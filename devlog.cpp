// Copyright 2017-2018 Leland Lucius
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>
#include <sys/errno.h>
#include <sys/reent.h>

#include "esp_heap_caps_init.h"
#include "esp_vfs.h"
#include "lwip/udp.h"

#include "devlog.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static FILE *dest_file = NULL;
static struct udp_pcb *dest_pcb = NULL;
static ip_addr_t dest_ipa = {};
static int dest_port = 0;

static char *log_buf = NULL;
static int log_pos = 0;
static int log_len = 0;

#define STDOUT_FD 1
#define STDERR_FD 2

static FILE *f_stdout = NULL;
static FILE *f_stderr = NULL;
static FILE *f_fwdout = NULL;
static FILE *f_fwderr = NULL;

// Will be returned to heap in DevLog ctor
#define EARLY_BUFFER_SIZE 4096
static struct
{
    int pos;
    int dropped;
    char buf[EARLY_BUFFER_SIZE];
} early = {};

static void early_putc(char c)
{
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        if (early.pos >= 0 && early.pos < sizeof(early.buf))
        {
            if (c != '\r')
            {
                early.buf[early.pos++] = c;
            }
        }
        else
        {
            early.dropped++;
        }

        vPortCPUReleaseMutex(&lock);
    }
}

// Lock should already be acquired before calling this function
static void devlog_putc(char c)
{
    if (log_buf == NULL || log_pos == log_len)
    {
        char *p = (char *) realloc(log_buf, log_len + 160);
        if (p != NULL)
        {
            log_buf = p;
            log_len += 160;
        }
    }
 
    if (log_buf)
    {
        if (c != '\r' && log_pos < log_len)
        {
            log_buf[log_pos++] = c;
        }

        if (c == '\n' && (dest_pcb || dest_file))
        {
            for (int i = 0, s = 0; i < log_pos; ++i)
            {
                if (log_buf[i] == '\n')
                {
                    int size = i - s;

                    if (size)
                    {
                        if (dest_pcb)
                        {
                            struct pbuf *pbuf = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
                            if (pbuf)
                            {   
                                pbuf_take(pbuf, &log_buf[s], size);
                                udp_sendto(dest_pcb, pbuf, &dest_ipa, dest_port);
                                pbuf_free(pbuf);
                            }
                        }

                        if (dest_file)
                        {
                            fwrite(&log_buf[s], 1, size, dest_file);
                        }
                    }

                    s = i + 1;
                    if (s < log_pos)
                    {
                        vTaskDelay(1);
                    }
                }
            }
            log_pos = 0;

            if (log_len > 160)
            {
                char *p;

                p = (char *) realloc(log_buf, 160);
                if (p != NULL)
                {
                    log_buf = p;
                    log_len = 160;
                }
            }
        }
    }
}

static void devlog_ets_putc(char c)
{
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        devlog_putc(c);
        
        vPortCPUReleaseMutex(&lock);
    }
}

static ssize_t devlog_vfs_write(int fd, const void *data, size_t size)
{
    if (fd != STDOUT_FD && fd != STDERR_FD)
    {
        errno = EBADF;
        return -1;
    }

    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        for (int n = 0; n < size; n++)
        {
            devlog_putc(((const char *) data)[n]);
        }

        vPortCPUReleaseMutex(&lock);
    }

    if (fd == STDOUT_FD && f_fwdout != NULL)
    {
        size = fwrite(data, 1, size, f_fwdout);
        fflush(f_fwdout);
    }
    else if (fd == STDERR_FD && f_fwderr != NULL)
    {
        size = fwrite(data, 1, size, f_fwderr);
        fflush(f_fwderr);
    }

    return size;
}

static int devlog_vfs_open(const char *path, int flags, int mode)
{
    if (path == NULL)
    {
        errno = EINVAL;
    }
    else if (f_stdout && f_stderr)
    {
        errno = ENFILE;
    }
    else if (strcmp(path, "/o") == 0)
    {
        return STDOUT_FD;
    }
    else if (strcmp(path, "/e") == 0)
    {
        return STDERR_FD;
    }
    else
    {
        errno = ENOENT;
    }

    return -1;
}

static void devlog_full_init()
{
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        ets_install_putc2(NULL);

        char tmp[64] = {0};
        int len = early.pos;

        if (early.dropped)
        {
            len += sprintf(tmp,
                           "\n*** %d bytes dropped during early logging\n",
                           early.dropped);
        }

        log_buf = (char *) malloc(len + 160);
        if (log_buf)
        {
            memcpy(log_buf, early.buf, early.pos);
            if (early.dropped)
            {
                strcpy(&log_buf[early.pos], tmp);
            }
            log_pos = len;
            log_len = len + 160;
        }

        // Return the early logging storage to the heap
        heap_caps_add_region((intptr_t)&early, (intptr_t)&early + sizeof(early));

        ets_install_putc2(devlog_ets_putc);

        vPortCPUReleaseMutex(&lock);
    }

    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.write = &devlog_vfs_write;
    vfs.open = &devlog_vfs_open;

    esp_vfs_register("/dev/log", &vfs, NULL);

    f_stdout = fopen("/dev/log/o", "w");
    if (f_stdout != NULL)
    {
        setvbuf(f_stdout, NULL, _IOLBF, 0);
        f_stderr = fopen("/dev/log/e", "w");
        if (f_stderr != NULL)
        {
            setvbuf(f_stdout, NULL, _IOLBF, 0);

            f_fwdout = _GLOBAL_REENT->_stdout;
            _GLOBAL_REENT->_stdout = f_stdout;

            f_fwderr = _GLOBAL_REENT->_stderr;
            _GLOBAL_REENT->_stderr = f_stderr;
        }
        else
        {
            fclose(f_stdout);
            f_stdout = NULL;
        }
    }
}

class DevLog
{
public:
    DevLog()
    {
        devlog_full_init();
    }

    virtual ~DevLog()
    {
    }
};
static DevLog initializer; 

#if CONFIG_FREERTOS_UNICORE
extern void start_cpu0_default(void) IRAM_ATTR __attribute__((noreturn));
void start_cpu0(void)
{
    ets_install_putc2(early_putc);
    start_cpu0_default();
}
#else
extern void start_cpu1_default(void) IRAM_ATTR __attribute__((noreturn));
void start_cpu1(void)
{
    ets_install_putc2(early_putc);
    start_cpu1_default();
}
#endif

int devlog_set_file_destination(const char *filename)
{
    FILE *f = NULL;

    if (filename != NULL)
    {
        FILE *f = fopen(filename, "a");
        if (f == NULL)
        {
            // errno should already be set
            return -1;
        }
    }

    FILE *old = NULL;
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        old = dest_file;

        dest_file = f;

        vPortCPUReleaseMutex(&lock);
    }

    if (old)
    {
        fclose(old);
    }

    return 0;
}

int devlog_set_udp_destination(const char *addr, int port)
{
    extern sys_thread_t g_lwip_task;
    if (g_lwip_task == NULL)
    {
        errno = EAGAIN;
        return -1;
    }

    ip_addr_t ipa;
    struct udp_pcb *pcb = NULL;
    if (addr != NULL)
    {
        if (!ipaddr_aton(addr, &ipa) || port == 0)
        {
            errno = EINVAL;
            return -1;
        }

        pcb = udp_new();
        if (pcb == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
    }

    struct udp_pcb *old = NULL;
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        old = dest_pcb;

        dest_pcb = pcb;        
        dest_ipa = ipa;
        dest_port = port;

        vPortCPUReleaseMutex(&lock);
    }

    if (old)
    {
        udp_remove(old);
    }

    return 0;
}

