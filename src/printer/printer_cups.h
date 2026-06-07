/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - CUPS queue management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PRINTER_CUPS_H
#define PRINTER_CUPS_H

#include <stdint.h>

/**
 * Create a CUPS printer queue for a redirected printer.
 *
 * @param queue_name   Sanitized CUPS queue name
 * @param session_id   Session ID (used in device URI)
 * @return 0 on success, -1 on failure
 */
int printer_cups_create_queue(const char *queue_name, int session_id,
                              unsigned int device_id);

/**
 * Remove a CUPS printer queue.
 *
 * @param queue_name  The CUPS queue name to remove
 * @return 0 on success, -1 on failure
 */
int printer_cups_remove_queue(const char *queue_name);

/**
 * Remove all stale xrdp printer queues for a specific session.
 * Called at startup to clean up queues left behind by crashes/reboots.
 *
 * @param session_id  Only remove queues whose device URI matches this session
 */
void printer_cups_cleanup_stale_queues(int session_id);

/**
 * Set a CUPS printer queue as the default for the session.
 *
 * @param queue_name  The CUPS queue name
 * @return 0 on success, -1 on failure
 */
int printer_cups_set_default(const char *queue_name);

/**
 * Initialize the CUPS listener socket for receiving print jobs
 * from the CUPS backend.
 *
 * @param session_id  Session ID
 * @return socket fd on success, -1 on failure
 */
int printer_cups_init_listener(int session_id);

/**
 * Close the CUPS listener socket.
 *
 * @param sock_fd  Socket fd to close
 */
void printer_cups_close_listener(int sock_fd);

#endif /* PRINTER_CUPS_H */
