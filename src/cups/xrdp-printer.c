/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * CUPS backend for xrdp printer redirection.
 *
 * This program is installed as a CUPS backend (/usr/lib/cups/backend/xrdp-printer).
 * When CUPS processes a print job on an xrdp virtual printer, it invokes this
 * backend which communicates the job data to chansrv for sending to the client.
 *
 * CUPS Backend Interface:
 *   backend job-id user title copies options [file]
 *
 * Communication with chansrv:
 *   Uses a Unix domain socket at /run/xrdp/sockdir/printer_<session_id>
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
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

#define BACKEND_NAME    "xrdp-printer"
#define SOCKET_DIR      "/run/xrdp/sockdir"
#define MAX_READ_BUF    65536

/* Message types for communication with chansrv */
#define MSG_TYPE_JOB_START   1
#define MSG_TYPE_JOB_DATA    2
#define MSG_TYPE_JOB_END     3
#define MSG_TYPE_JOB_ACK     4

/* Message header sent to chansrv */
struct msg_header
{
    uint32_t msg_type;
    uint32_t device_id;
    uint32_t data_len;
    uint32_t job_id;
};

/**
 * Get the session ID from the environment.
 * CUPS backends run as the user who submitted the job, and the
 * XRDP_SESSION environment variable is set by our PAM/session setup.
 */
static int get_session_id(void)
{
    const char *session_str = getenv("XRDP_SESSION");
    if (session_str != NULL)
    {
        return atoi(session_str);
    }

    /* Fallback: try to get from DISPLAY variable (xrdp sets :10, :11, etc.) */
    const char *display = getenv("DISPLAY");
    if (display != NULL && display[0] == ':')
    {
        return atoi(display + 1);
    }

    return -1;
}

/**
 * Parse the CUPS device URI to extract session_id and device_id.
 * URI format: xrdp-printer://<session_id>/<device_id_hex>
 */
static int parse_device_uri(int *out_session_id, uint32_t *out_device_id)
{
    const char *uri = getenv("DEVICE_URI");
    if (uri == NULL)
    {
        return -1;
    }

    const char *prefix = "xrdp-printer://";
    if (strncmp(uri, prefix, strlen(prefix)) != 0)
    {
        return -1;
    }

    const char *p = uri + strlen(prefix);
    char *slash = strchr(p, '/');
    if (slash == NULL)
    {
        /* Legacy format: xrdp-printer://<session_id> - no device_id */
        *out_session_id = atoi(p);
        *out_device_id = 0;
        return -1;
    }

    *out_session_id = atoi(p);
    *out_device_id = (uint32_t)strtoul(slash + 1, NULL, 16);
    return 0;
}

/**
 * Connect to the chansrv printer socket
 */
static int connect_to_chansrv(int session_id)
{
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME " - socket() failed: %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s/printer_%d", SOCKET_DIR, session_id);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME " - connect(%s) failed: %s\n",
                addr.sun_path, strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

/**
 * Send a message header to chansrv
 */
static int send_msg(int sock, uint32_t msg_type, uint32_t device_id,
                    uint32_t job_id, const void *data, uint32_t data_len)
{
    struct msg_header hdr;
    hdr.msg_type = msg_type;
    hdr.device_id = device_id;
    hdr.data_len = data_len;
    hdr.job_id = job_id;

    /* Send header */
    ssize_t n = write(sock, &hdr, sizeof(hdr));
    if (n != sizeof(hdr))
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME " - failed to send header: %s\n",
                strerror(errno));
        return -1;
    }

    /* Send data if any */
    if (data != NULL && data_len > 0)
    {
        const uint8_t *p = (const uint8_t *)data;
        uint32_t remaining = data_len;
        while (remaining > 0)
        {
            n = write(sock, p, remaining);
            if (n <= 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                fprintf(stderr, "ERROR: " BACKEND_NAME
                        " - failed to send data: %s\n", strerror(errno));
                return -1;
            }
            p += n;
            remaining -= (uint32_t)n;
        }
    }

    return 0;
}

/**
 * Wait for acknowledgement from chansrv
 */
static int wait_for_ack(int sock)
{
    struct msg_header ack;
    ssize_t n = read(sock, &ack, sizeof(ack));
    if (n != sizeof(ack))
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME " - failed to read ack: %s\n",
                strerror(errno));
        return -1;
    }

    if (ack.msg_type != MSG_TYPE_JOB_ACK)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME
                " - unexpected ack type: %u\n", ack.msg_type);
        return -1;
    }

    return 0;
}

/**
 * Main backend processing: read print data and send to chansrv
 */
static int process_job(int input_fd, int sock, uint32_t device_id,
                       uint32_t job_id)
{
    uint8_t buf[MAX_READ_BUF];
    ssize_t bytes_read;
    uint64_t total_bytes = 0;

    /* Send job start */
    if (send_msg(sock, MSG_TYPE_JOB_START, device_id, job_id, NULL, 0) != 0)
    {
        return -1;
    }

    /* Read and forward print data */
    while ((bytes_read = read(input_fd, buf, sizeof(buf))) > 0)
    {
        if (send_msg(sock, MSG_TYPE_JOB_DATA, device_id, job_id,
                     buf, (uint32_t)bytes_read) != 0)
        {
            return -1;
        }
        total_bytes += (uint64_t)bytes_read;
    }

    if (bytes_read < 0)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME " - read failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* Send job end */
    if (send_msg(sock, MSG_TYPE_JOB_END, device_id, job_id, NULL, 0) != 0)
    {
        return -1;
    }

    /* Wait for chansrv to acknowledge the job is sent to client */
    if (wait_for_ack(sock) != 0)
    {
        return -1;
    }

    fprintf(stderr, "INFO: " BACKEND_NAME " - job %u complete, %lu bytes\n",
            job_id, (unsigned long)total_bytes);

    return 0;
}

/**
 * Print discovery information (when called with no arguments).
 * CUPS calls the backend with no args to discover available devices.
 */
static void print_device_info(void)
{
    /* Output format: device-class scheme "Unknown" "info" */
    printf("network " BACKEND_NAME " \"Unknown\" "
           "\"xrdp Printer Redirection Backend\"\n");
}

int main(int argc, char *argv[])
{
    int input_fd;
    int sock;
    int session_id;
    uint32_t device_id;
    uint32_t job_id;
    int ret = 1;

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /*
     * CUPS Backend calling conventions:
     *   0 args: discovery mode - list available devices
     *   5 args: job-id user title copies options (data on stdin)
     *   6 args: job-id user title copies options filename
     */
    if (argc == 1)
    {
        /* Discovery mode */
        print_device_info();
        return 0;
    }

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: " BACKEND_NAME
                " job-id user title copies options [file]\n");
        return 1;
    }

    job_id = (uint32_t)atoi(argv[1]);

    /* Determine input source */
    if (argc == 7)
    {
        input_fd = open(argv[6], O_RDONLY);
        if (input_fd < 0)
        {
            fprintf(stderr, "ERROR: " BACKEND_NAME
                    " - unable to open file '%s': %s\n",
                    argv[6], strerror(errno));
            return 1;
        }
    }
    else
    {
        input_fd = STDIN_FILENO;
    }

    /* Get session and device info from DEVICE_URI */
    if (parse_device_uri(&session_id, &device_id) != 0)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME
                " - cannot parse DEVICE_URI\n");
        goto cleanup;
    }

    if (session_id < 0)
    {
        /* Fallback: try DISPLAY variable */
        session_id = get_session_id();
        if (session_id < 0)
        {
            fprintf(stderr, "ERROR: " BACKEND_NAME
                    " - cannot determine session ID\n");
            goto cleanup;
        }
    }

    if (device_id == 0)
    {
        fprintf(stderr, "ERROR: " BACKEND_NAME
                " - cannot determine device ID from URI\n");
        goto cleanup;
    }

    /* Connect to chansrv */
    sock = connect_to_chansrv(session_id);
    if (sock < 0)
    {
        goto cleanup;
    }

    /* Process the print job */
    if (process_job(input_fd, sock, device_id, job_id) == 0)
    {
        ret = 0;
    }

    close(sock);

cleanup:
    if (input_fd != STDIN_FILENO)
    {
        close(input_fd);
    }

    return ret;
}
