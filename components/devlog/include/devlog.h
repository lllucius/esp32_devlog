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

#if !defined(_DEVLOG_H_)
#define _DEVLOG_H_ 1

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sets the log destination to a file.
 *
 * @param file Opened file where all messages will be written.  Pass NULL to unset.
 *             NOTE: Do not close the file unless you unset it first.
 *
 * @return 0 upon success, -1 for failure (check errno for reason)
 *
 * Example usage:
 * @code{c}
 *      // Set the destination
 *      FILE *log = fopen("/sdcard/logfile", "a");
 *      devlog_set_file_destination(log);
 *
 *      // Unset the destination
 *      devlog_set_file_destination(NULL);
 *      fclose(log);
 * @endcode
 */
int devlog_set_file_destination(FILE *file);

/**
 * @brief Writes all log messages to the specified (UDP) IP and port. 
 *
 * @param addr The IP address (xxx.xxx.xxx.xxx) of the destination host.  Pass
 *             NULL to unset a previously set destination.
 * @param port The destination port.
 *
 * @return 0 upon success, -1 for failure (check errno for reason)
 *
 * Example usage:
 * @code{c}
 *      // Set the destination
 *      devlog_set_udp_destination("192.168.2.1", 514);
 *
 *      // Unset the destination
 *      devlog_set_udp_destination(NULL, 0);
 * @endcode
 */
int devlog_set_udp_destination(const char *addr, int port);

/**
 * @brief Sets the size of the retention buffer
 *
 * @param size The number of bytes to retain in a local buffer.  The
 *             default is 0 which doesn't retain anything.
 *
 * @return 0 upon success, -1 for failure (check errno for reason)
 *
 * Example usage:
 * @code{c}
 *      // Set the destination
 *      devlog_set_retention_destination(256);
 *
 *      // Retrieve the current buffer contents
 *      char buffer[256];
 *      devlog_get_retention_content(buffer, sizeof(buffer));
 * @endcode
 */

int devlog_set_retention_destination(int size);

/**
 * @brief Retrieves the contents of the retention buffer
 *
 * @param dest  Pointer to the destination buffer.
 * @param size  The size of the destination buffer.
 * @param clear true=clear retention buffer, false=keep content
 *
 * @return Number of bytes copied, -1 for failure
 *
 * Example usage:
 * @code{c}
 *      // Set the destination
 *      devlog_set_retention_destination(256);
 *
 *      // Retrieve the current buffer contents
 *      char buffer[256];
 *      devlog_get_retention_content(buffer, sizeof(buffer), false);
 * @endcode
 */
int devlog_get_retention_content(char *dest, int size, bool clear);

#ifdef __cplusplus
}
#endif

#endif
