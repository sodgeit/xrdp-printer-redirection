/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - socket handler for CUPS backend communication
 *
 * This module handles Unix domain socket connections from the CUPS
 * backend (xrdp-printer). It receives print job data and dispatches
 * it to the devredir_printer module for sending to the RDP client.
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

#ifndef PRINTER_SOCKET_H
#define PRINTER_SOCKET_H

#include <stdint.h>

/* Message types matching the CUPS backend protocol */
#define MSG_TYPE_JOB_START   1
#define MSG_TYPE_JOB_DATA    2
#define MSG_TYPE_JOB_END     3
#define MSG_TYPE_JOB_ACK     4

/* Message header (must match xrdp-printer.c backend) */
struct msg_header
{
    uint32_t msg_type;
    uint32_t device_id;
    uint32_t data_len;
    uint32_t job_id;
};

/* Maximum number of simultaneous backend connections */
#define MAX_BACKEND_CONNECTIONS 8

/* Buffer for accumulating job data from a backend connection */
struct backend_conn
{
    int fd;
    uint32_t device_id;
    uint32_t job_id;
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_capacity;
    int active;
};

/**
 * Initialize the socket handler state.
 */
void printer_socket_init(void);

/**
 * Handle activity on the listener socket (new connections).
 *
 * @param listener_fd  The listening socket fd
 * @return 0 on success, -1 on failure
 */
int printer_socket_accept(int listener_fd);

/**
 * Handle data available on a backend connection.
 * Reads messages and accumulates job data.
 * When a job is complete (MSG_TYPE_JOB_END), dispatches it for sending.
 *
 * @param conn  The backend connection to process
 * @return 0 on success, -1 on connection close/error
 */
int printer_socket_handle_data(struct backend_conn *conn);

/**
 * Get all active connection fds for polling.
 *
 * @param fds     Array to fill with active fds
 * @param max_fds Maximum size of the array
 * @return number of active fds added
 */
int printer_socket_get_fds(int *fds, int max_fds);

/**
 * Check all active connections for data and process them.
 */
void printer_socket_check_all(void);

/**
 * Close all backend connections and free resources.
 */
void printer_socket_cleanup(void);

#endif /* PRINTER_SOCKET_H */
