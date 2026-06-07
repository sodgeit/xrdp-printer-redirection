/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - CUPS queue management implementation
 *
 * This module manages CUPS printer queues for redirected printers.
 * It creates virtual queues that use the xrdp-printer backend to
 * route print jobs back to the RDP client.
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
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "printer_cups.h"

#define LOG_PREFIX "CUPS_MGR: "
#define SOCKET_DIR "/run/xrdp/sockdir"
#define PPD_PATH "/usr/share/ppd/xrdp/xrdp-printer.ppd"
#define BACKEND_PATH "/usr/lib/cups/backend/xrdp-printer"

/**
 * Create a CUPS printer queue using lpadmin.
 *
 * The queue uses:
 *   - Backend: xrdp-printer (our custom CUPS backend)
 *   - Device URI: xrdp-printer://<device_id_hex>
 *   - PPD: Our generic PostScript PPD
 *   - Shared: No (per-session printer)
 */
int printer_cups_create_queue(const char *queue_name, int session_id,
                              unsigned int device_id)
{
    char cmd[512];
    int ret;

    if (queue_name == NULL || queue_name[0] == '\0')
    {
        return -1;
    }

    /* Verify backend exists */
    if (access(BACKEND_PATH, X_OK) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Backend not found at %s\n", BACKEND_PATH);
        return -1;
    }

    /*
     * Create the printer queue with lpadmin:
     *   -p <name>          printer name
     *   -v <uri>           device URI (session_id/device_id_hex)
     *   -P <ppd>           PPD file (or use -m raw for RAW-only)
     *   -o printer-is-shared=false   don't share on network
     *   -E                 enable and accept jobs
     */
    snprintf(cmd, sizeof(cmd),
             "lpadmin -p '%s' "
             "-v 'xrdp-printer://%d/%x' "
             "-P '%s' "
             "-o printer-is-shared=false "
             "-E 2>&1",
             queue_name, session_id, device_id, PPD_PATH);

    fprintf(stderr, LOG_PREFIX "Running: %s\n", cmd);
    ret = system(cmd);
    if (ret != 0)
    {
        fprintf(stderr, LOG_PREFIX "lpadmin (PPD) failed with rc=%d\n", ret);
        /* Fallback: try creating as RAW printer (no PPD needed) */
        snprintf(cmd, sizeof(cmd),
                 "lpadmin -p '%s' "
                 "-v 'xrdp-printer://%d/%x' "
                 "-m raw "
                 "-o printer-is-shared=false "
                 "-E 2>&1",
                 queue_name, session_id, device_id);

        fprintf(stderr, LOG_PREFIX "Running: %s\n", cmd);
        ret = system(cmd);
        if (ret != 0)
        {
            fprintf(stderr, LOG_PREFIX "Failed to create CUPS queue '%s'"
                    " (rc=%d)\n", queue_name, ret);
            return -1;
        }
    }

    /* Enable the queue */
    snprintf(cmd, sizeof(cmd), "cupsenable '%s' 2>/dev/null", queue_name);
    if (system(cmd) != 0) { /* non-critical */ }

    snprintf(cmd, sizeof(cmd), "cupsaccept '%s' 2>/dev/null", queue_name);
    if (system(cmd) != 0) { /* non-critical */ }

    /* Restrict queue access to the session user only.
     * For per-session cupsd this is a no-op (private instance allows all).
     * For system cupsd this provides user isolation. */
    {
        const char *user = getenv("USER");
        if (user != NULL && user[0] != '\0')
        {
            snprintf(cmd, sizeof(cmd),
                     "lpadmin -p '%s' -u allow:'%s' 2>/dev/null",
                     queue_name, user);
            if (system(cmd) != 0) { /* non-critical */ }
        }
    }

    fprintf(stderr, LOG_PREFIX "Created CUPS queue '%s'\n", queue_name);
    return 0;
}

/**
 * Remove a CUPS printer queue.
 * Retries once after a short delay if the first attempt fails,
 * since CUPS can reject requests when its scheduler is busy.
 */
int printer_cups_remove_queue(const char *queue_name)
{
    char cmd[256];
    int ret;
    int attempts = 0;

    if (queue_name == NULL || queue_name[0] == '\0')
    {
        return -1;
    }

    /* Cancel any pending jobs first */
    snprintf(cmd, sizeof(cmd), "cancel -a '%s' 2>/dev/null", queue_name);
    if (system(cmd) != 0) { /* non-critical */ }

    /* Remove the queue, retry once on failure */
    snprintf(cmd, sizeof(cmd), "lpadmin -x '%s' 2>/dev/null", queue_name);
    while (attempts < 2)
    {
        ret = system(cmd);
        if (ret == 0)
        {
            fprintf(stderr, LOG_PREFIX "Removed CUPS queue '%s'\n",
                    queue_name);
            return 0;
        }
        attempts++;
        if (attempts < 2)
        {
            usleep(100000); /* 100ms before retry */
        }
    }

    fprintf(stderr, LOG_PREFIX "Failed to remove CUPS queue '%s'\n",
            queue_name);
    return -1;
}

/**
 * Remove stale xrdp printer queues.
 *
 * If session_id > 0 (system cupsd mode): removes only queues matching
 *   "xrdp_<session_id>_" prefix (this session's queues on system cupsd).
 * If session_id <= 0 (per-session cupsd mode): removes all "xrdp_" queues
 *   since the entire cupsd instance belongs to this session.
 */
void printer_cups_cleanup_stale_queues(int session_id)
{
    FILE *fp;
    char line[512];
    char prefix[32];
    size_t prefix_len;

    if (session_id > 0)
    {
        snprintf(prefix, sizeof(prefix), "xrdp_%d_", session_id);
    }
    else
    {
        snprintf(prefix, sizeof(prefix), "xrdp_");
    }
    prefix_len = strlen(prefix);

    /* List all printers with their device URIs */
    fp = popen("lpstat -v 2>/dev/null", "r");
    if (fp == NULL)
    {
        return;
    }

    /* Output format: "device for <name>: <uri>" */
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *name_start;
        char *name_end;
        char queue_name[256];

        /* Find queue name between "device for " and ":" */
        name_start = strstr(line, "device for ");
        if (name_start == NULL)
        {
            continue;
        }
        name_start += 11; /* skip "device for " */

        name_end = strchr(name_start, ':');
        if (name_end == NULL)
        {
            continue;
        }

        /* Extract queue name */
        size_t name_len = (size_t)(name_end - name_start);
        if (name_len >= sizeof(queue_name))
        {
            continue;
        }
        memcpy(queue_name, name_start, name_len);
        queue_name[name_len] = '\0';

        /* Check if queue name starts with xrdp_ */
        if (strncmp(queue_name, prefix, prefix_len) == 0)
        {
            fprintf(stderr, LOG_PREFIX "Removing stale queue '%s'\n",
                    queue_name);
            printer_cups_remove_queue(queue_name);
        }
    }

    pclose(fp);
}

/**
 * Set a CUPS printer as the default
 */
int printer_cups_set_default(const char *queue_name)
{
    char cmd[256];

    if (queue_name == NULL || queue_name[0] == '\0')
    {
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "lpoptions -d '%s' 2>/dev/null", queue_name);
    return (system(cmd) == 0) ? 0 : -1;
}

/**
 * Initialize the Unix domain socket listener for receiving
 * print job data from the CUPS backend.
 */
int printer_cups_init_listener(int session_id)
{
    int sock;
    struct sockaddr_un addr;

    /* Ensure socket directory exists */
    if (mkdir(SOCKET_DIR, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr, LOG_PREFIX "Failed to create socket dir: %s\n",
                strerror(errno));
        return -1;
    }

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, LOG_PREFIX "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s/printer_%d", SOCKET_DIR, session_id);

    /* Remove stale socket */
    unlink(addr.sun_path);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, LOG_PREFIX "bind(%s) failed: %s\n",
                addr.sun_path, strerror(errno));
        close(sock);
        return -1;
    }

    /* Allow the CUPS backend (running as lp user) to connect */
    chmod(addr.sun_path, 0666);

    if (listen(sock, 5) < 0)
    {
        fprintf(stderr, LOG_PREFIX "listen() failed: %s\n", strerror(errno));
        close(sock);
        unlink(addr.sun_path);
        return -1;
    }

    /* Set non-blocking so accept() won't hang the xrdp event loop */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    /* Set close-on-exec so forked processes (system() calls) don't inherit */
    int fdflags = fcntl(sock, F_GETFD, 0);
    if (fdflags >= 0)
    {
        fcntl(sock, F_SETFD, fdflags | FD_CLOEXEC);
    }

    fprintf(stderr, LOG_PREFIX "Listening on %s\n", addr.sun_path);
    return sock;
}

/**
 * Close the listener socket and remove the socket file
 */
void printer_cups_close_listener(int sock_fd)
{
    if (sock_fd >= 0)
    {
        close(sock_fd);
    }
}
