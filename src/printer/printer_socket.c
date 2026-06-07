/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - socket handler implementation
 *
 * Handles Unix domain socket connections from the CUPS backend.
 * Protocol:
 *   Backend connects -> sends MSG_TYPE_JOB_START
 *   Backend sends MSG_TYPE_JOB_DATA (one or more times with print data)
 *   Backend sends MSG_TYPE_JOB_END
 *   Server sends MSG_TYPE_JOB_ACK after job is sent to client
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>

#include "printer_socket.h"
#include "printer.h"
#include "../devredir_printer.h"

#define LOG_PREFIX "PRN_SOCK: "

/* Active backend connections */
static struct backend_conn g_conns[MAX_BACKEND_CONNECTIONS];
static int g_conn_count = 0;

/**
 * Initialize socket handler state
 */
void printer_socket_init(void)
{
    memset(g_conns, 0, sizeof(g_conns));
    g_conn_count = 0;
}

/**
 * Find a free connection slot
 */
static struct backend_conn *find_free_slot(void)
{
    for (int i = 0; i < MAX_BACKEND_CONNECTIONS; i++)
    {
        if (!g_conns[i].active)
        {
            return &g_conns[i];
        }
    }
    return NULL;
}

/**
 * Accept a new connection from the CUPS backend
 */
int printer_socket_accept(int listener_fd)
{
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(listener_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0; /* No pending connections */
        }
        fprintf(stderr, LOG_PREFIX "accept() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct backend_conn *conn = find_free_slot();
    if (conn == NULL)
    {
        fprintf(stderr, LOG_PREFIX "Too many connections, rejecting\n");
        close(fd);
        return 0;
    }

    memset(conn, 0, sizeof(struct backend_conn));
    conn->fd = fd;
    conn->active = 1;
    conn->data = NULL;
    conn->data_len = 0;
    conn->data_capacity = 0;
    g_conn_count++;

    fprintf(stderr, LOG_PREFIX "Accepted backend connection (fd=%d)\n", fd);
    return 0;
}

/**
 * Read exactly n bytes from a socket (blocking helper)
 */
static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0)
    {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0)
        {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                /*
                 * Non-blocking fd has no more data right now.
                 * Since messages from the backend are small and written
                 * atomically, partial reads shouldn't happen in practice.
                 * Wait briefly (100ms max) for the rest to arrive.
                 */
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                if (poll(&pfd, 1, 100) <= 0)
                {
                    return -1;
                }
                continue;
            }
            return -1; /* EOF or error */
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/**
 * Send an ACK message back to the backend
 */
static int send_ack(int fd, uint32_t device_id, uint32_t job_id)
{
    struct msg_header ack;
    ack.msg_type = MSG_TYPE_JOB_ACK;
    ack.device_id = device_id;
    ack.data_len = 0;
    ack.job_id = job_id;

    ssize_t n = write(fd, &ack, sizeof(ack));
    return (n == sizeof(ack)) ? 0 : -1;
}

/**
 * Ensure the data buffer has enough capacity
 */
static int ensure_capacity(struct backend_conn *conn, uint32_t additional)
{
    uint32_t needed = conn->data_len + additional;
    if (needed <= conn->data_capacity)
    {
        return 0;
    }

    /* Grow to next power of 2 or at least needed size */
    uint32_t new_cap = conn->data_capacity;
    if (new_cap == 0)
    {
        new_cap = 65536;
    }
    while (new_cap < needed)
    {
        new_cap *= 2;
        if (new_cap > 256 * 1024 * 1024) /* 256MB sanity limit */
        {
            fprintf(stderr, LOG_PREFIX "Job data exceeds 256MB limit\n");
            return -1;
        }
    }

    uint8_t *new_buf = realloc(conn->data, new_cap);
    if (new_buf == NULL)
    {
        fprintf(stderr, LOG_PREFIX "realloc failed for %u bytes\n", new_cap);
        return -1;
    }
    conn->data = new_buf;
    conn->data_capacity = new_cap;
    return 0;
}

/**
 * Handle data on a backend connection
 */
int printer_socket_handle_data(struct backend_conn *conn)
{
    struct msg_header hdr;

    if (!conn->active)
    {
        return -1;
    }

    /* Read message header */
    if (read_exact(conn->fd, &hdr, sizeof(hdr)) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Failed to read header (fd=%d)\n", conn->fd);
        return -1;
    }

    switch (hdr.msg_type)
    {
        case MSG_TYPE_JOB_START:
            conn->device_id = hdr.device_id;
            conn->job_id = hdr.job_id;
            conn->data_len = 0;
            fprintf(stderr, LOG_PREFIX "Job %u started for device 0x%x\n",
                    hdr.job_id, hdr.device_id);
            break;

        case MSG_TYPE_JOB_DATA:
            if (hdr.data_len > 0)
            {
                if (ensure_capacity(conn, hdr.data_len) != 0)
                {
                    return -1;
                }

                if (read_exact(conn->fd, conn->data + conn->data_len,
                               hdr.data_len) != 0)
                {
                    fprintf(stderr, LOG_PREFIX
                            "Failed to read job data (%u bytes)\n",
                            hdr.data_len);
                    return -1;
                }
                conn->data_len += hdr.data_len;
            }
            break;

        case MSG_TYPE_JOB_END:
            fprintf(stderr, LOG_PREFIX
                    "Job %u complete: %u bytes for device 0x%x\n",
                    conn->job_id, conn->data_len, conn->device_id);

            /* Dispatch the job to the devredir printer module */
            if (conn->data_len > 0 && conn->data != NULL)
            {
                int rc = devredir_printer_send_job(conn->device_id,
                                                    conn->data,
                                                    conn->data_len);
                if (rc != 0)
                {
                    fprintf(stderr, LOG_PREFIX
                            "Failed to send job %u to client\n",
                            conn->job_id);
                }
            }

            /* Send ACK to backend */
            send_ack(conn->fd, conn->device_id, conn->job_id);

            /* Reset for potential next job on same connection */
            conn->data_len = 0;
            break;

        default:
            fprintf(stderr, LOG_PREFIX "Unknown msg_type %u\n", hdr.msg_type);
            return -1;
    }

    return 0;
}

/**
 * Close a backend connection
 */
static void close_conn(struct backend_conn *conn)
{
    if (!conn->active)
    {
        return;
    }

    close(conn->fd);
    free(conn->data);
    memset(conn, 0, sizeof(struct backend_conn));
    g_conn_count--;
}

/**
 * Get all active connection fds for polling
 */
int printer_socket_get_fds(int *fds, int max_fds)
{
    int count = 0;
    for (int i = 0; i < MAX_BACKEND_CONNECTIONS && count < max_fds; i++)
    {
        if (g_conns[i].active)
        {
            fds[count++] = g_conns[i].fd;
        }
    }
    return count;
}

/**
 * Check all active connections for available data
 */
void printer_socket_check_all(void)
{
    for (int i = 0; i < MAX_BACKEND_CONNECTIONS; i++)
    {
        if (!g_conns[i].active)
        {
            continue;
        }

        /* Poll to see if data is available */
        struct pollfd pfd = { .fd = g_conns[i].fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 0); /* Non-blocking check */

        if (rc > 0 && (pfd.revents & POLLIN))
        {
            if (printer_socket_handle_data(&g_conns[i]) != 0)
            {
                fprintf(stderr, LOG_PREFIX
                        "Connection closed (fd=%d)\n", g_conns[i].fd);
                close_conn(&g_conns[i]);
            }
        }
        else if (rc > 0 && (pfd.revents & (POLLHUP | POLLERR)))
        {
            close_conn(&g_conns[i]);
        }
    }
}

/**
 * Close all backend connections
 */
void printer_socket_cleanup(void)
{
    for (int i = 0; i < MAX_BACKEND_CONNECTIONS; i++)
    {
        if (g_conns[i].active)
        {
            close_conn(&g_conns[i]);
        }
    }
    g_conn_count = 0;
}
