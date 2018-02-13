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

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct
{
    int start;
    int end;
    int active;
    int size;
    char buffer[1];
} ringbuf_t;

void rb_put(ringbuf_t *rb, char c)
{
    if (rb)
    {
        rb->buffer[rb->end] = c;
        rb->end = (rb->end + 1) % rb->size;

        if (rb->active < rb->size)
        {
            rb->active++;
        }
        else
        {
            rb->start = (rb->start + 1) % rb->size;
        }
    }
}

static bool rb_get(ringbuf_t *rb, char *p)
{
    if (rb == NULL || rb->active == 0)
    {
        return false;
    }

    *p = rb->buffer[rb->start];
    rb->start = (rb->start + 1) % rb->size;

    rb->active--;

    return true;
}

static int rb_get_all(ringbuf_t *rb, char *out, int size, bool clear)
{
    int cnt = 0;

    if (rb != NULL && rb->active != 0)
    {
        int pos = rb->start;

        for (int i = 0; i < rb->active; ++i)
        {
            if (i == size)
            {
                break;
            }
        
            *out++ = rb->buffer[pos];
            pos = (pos + 1) % rb->size;
            cnt++;
        }

        if (clear)
        {
            rb->start = 0;
            rb->end = 0;
            rb->active = 0;
        }
    }

    return cnt;
}

#if !defined(CONFIG_DEVLOG_EARLY_BUFFER_SIZE)
#define CONFIG_DEVLOG_EARLY_BUFFER_SIZE 4096
#endif

#if CONFIG_DEVLOG_EARLY_BUFFER_SIZE

// Will be returned to heap in DevLog ctor
static struct
{
    ringbuf_t rb;
    char buffer[CONFIG_DEVLOG_EARLY_BUFFER_SIZE];
}
early =
{
    .rb =
    {
        .start = 0,
        .end = 0,
        .active = 0,
        .size = CONFIG_DEVLOG_EARLY_BUFFER_SIZE,
        .buffer = {}
    },
    .buffer = {}
};
static ringbuf_t *dest_rb = &early.rb;

static void early_putc(char c)
{
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        if (c != '\r')
        {
            rb_put(dest_rb, c);
        }

        vPortCPUReleaseMutex(&lock);
    }
}

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

#else

static ringbuf_t *dest_rb = NULL;

#endif

// Lock should already be acquired before calling this function
static void devlog_putc(char c)
{
    if (c == '\r')
    {
        return;
    }

    if (dest_rb)
    {
         rb_put(dest_rb, c);
    }

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
        if (log_pos < log_len)
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

        int len = 0;

#if CONFIG_DEVLOG_EARLY_BUFFER_SIZE
        len += dest_rb->active;
#endif

        log_buf = (char *) malloc(len + 160);
        if (log_buf)
        {

#if CONFIG_DEVLOG_EARLY_BUFFER_SIZE
            rb_get_all(dest_rb, log_buf, len, true);
#endif

            log_pos = len;
            log_len = len + 160;
        }

#if CONFIG_DEVLOG_EARLY_BUFFER_SIZE
        // Return the early logging storage to the heap
        heap_caps_add_region((intptr_t)&early, (intptr_t)&early + sizeof(early));

        dest_rb = NULL;
#endif

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

int devlog_set_file_destination(FILE *file)
{
    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        dest_file = file;

        vPortCPUReleaseMutex(&lock);
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

    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        struct udp_pcb *old = dest_pcb;

        dest_pcb = pcb;        
        dest_ipa = ipa;
        dest_port = port;

        vPortCPUReleaseMutex(&lock);

        if (old)
        {
            udp_remove(old);
        }
    }

    return 0;
}

int devlog_set_retention_destination(int size)
{
    ringbuf_t *rb = (ringbuf_t *) malloc(sizeof(ringbuf_t) + size);
    if (rb == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        rb->start = 0;
        rb->end = 0;
        rb->active = 0;
        rb->size = size;

        if (dest_rb)
        {
            char c;
            while (rb_get(dest_rb, &c))
            {
                rb_put(rb, c);
            }

            free(dest_rb);
        }
        dest_rb = rb;

        vPortCPUReleaseMutex(&lock);
    }

    return 0;
}

int devlog_get_retention_content(char *dest, int size, bool clear)
{
    int cnt = 0;

    if (vPortCPUAcquireMutexTimeout(&lock, portMUX_NO_TIMEOUT))
    {
        cnt = rb_get_all(dest_rb, dest, size, clear);

        vPortCPUReleaseMutex(&lock);
    }

    return cnt;
}

#ifdef __cplusplus
}
#endif

// Singleton used to switch from early capture (if enabled) to full capture.
// It gets initialzed during boot after the memory system has been initialized.
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



