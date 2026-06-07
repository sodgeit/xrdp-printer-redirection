/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - device management implementation
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>

#include "printer.h"

#define LOG_PREFIX "PRINTER: "

static uint32_t g_next_job_id = 1;

/**
 * Initialize the printer manager for a session
 */
int printer_manager_init(struct printer_manager *mgr, int session_id)
{
    char *runtime_dir;

    if (mgr == NULL)
    {
        return -1;
    }

    memset(mgr, 0, sizeof(struct printer_manager));
    mgr->session_id = session_id;

    /* Use XDG_RUNTIME_DIR if available, otherwise /tmp */
    runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir != NULL && runtime_dir[0] != '\0')
    {
        snprintf(mgr->spool_dir, sizeof(mgr->spool_dir),
                 "%s/xrdp-printer", runtime_dir);
    }
    else
    {
        snprintf(mgr->spool_dir, sizeof(mgr->spool_dir),
                 "/tmp/xrdp-printer-%d", session_id);
    }

    /* Create spool directory */
    if (mkdir(mgr->spool_dir, 0700) != 0 && errno != EEXIST)
    {
        fprintf(stderr, LOG_PREFIX "Failed to create spool dir %s: %s\n",
                mgr->spool_dir, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Cleanup the printer manager, removing all printers and jobs
 */
void printer_manager_cleanup(struct printer_manager *mgr)
{
    if (mgr == NULL)
    {
        return;
    }

    printer_dev_remove_all(mgr);

    /* Remove session spool directory */
    rmdir(mgr->spool_dir);
}

/**
 * Sanitize a printer name for use as a CUPS queue name.
 * Replaces non-alphanumeric characters with underscores.
 *
 * If session_id <= 0 (per-session cupsd mode):
 *   Format: xrdp_<sanitized_name>
 *   Stable across sessions -- user preferences persist.
 *
 * If session_id > 0 (system cupsd mode):
 *   Format: xrdp_<session_id>_<sanitized_name>
 *   Includes display number to avoid conflicts between sessions.
 *
 * Caller must free the returned string.
 */
char *printer_sanitize_name(const char *name, int session_id)
{
    char prefix[32];

    if (session_id > 0)
    {
        snprintf(prefix, sizeof(prefix), "xrdp_%d_", session_id);
    }
    else
    {
        snprintf(prefix, sizeof(prefix), "xrdp_");
    }
    size_t prefix_len = strlen(prefix);

    if (name == NULL || name[0] == '\0')
    {
        char *result = malloc(prefix_len + 8);
        if (result == NULL)
        {
            return NULL;
        }
        snprintf(result, prefix_len + 8, "%sprinter", prefix);
        return result;
    }

    size_t len = strlen(name);
    char *sanitized = malloc(prefix_len + len + 1);
    if (sanitized == NULL)
    {
        return NULL;
    }

    memcpy(sanitized, prefix, prefix_len);

    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '-')
        {
            sanitized[prefix_len + i] = c;
        }
        else
        {
            sanitized[prefix_len + i] = '_';
        }
    }
    sanitized[prefix_len + len] = '\0';

    return sanitized;
}

/**
 * Add a printer device to the manager
 */
struct printer_dev *printer_dev_add(struct printer_manager *mgr,
                                    uint32_t device_id,
                                    const char *dos_name,
                                    uint32_t flags,
                                    const char *printer_name,
                                    const char *driver_name)
{
    if (mgr == NULL || mgr->printer_count >= PRINTER_MAX_PRINTERS)
    {
        return NULL;
    }

    /* Check for duplicate device_id */
    if (printer_dev_find(mgr, device_id) != NULL)
    {
        return NULL;
    }

    struct printer_dev *dev = calloc(1, sizeof(struct printer_dev));
    if (dev == NULL)
    {
        return NULL;
    }

    dev->device_id = device_id;
    dev->flags = flags;

    if (dos_name != NULL)
    {
        strncpy(dev->dos_name, dos_name, sizeof(dev->dos_name) - 1);
        dev->dos_name[sizeof(dev->dos_name) - 1] = '\0';
    }

    if (printer_name != NULL)
    {
        dev->printer_name = strdup(printer_name);
    }

    if (driver_name != NULL)
    {
        dev->driver_name = strdup(driver_name);
    }

    /*
     * Generate a CUPS-friendly queue name.
     * Per-session cupsd: pass 0 -> stable name (xrdp_<name>)
     * System cupsd: pass session_id -> unique name (xrdp_<id>_<name>)
     */
    const char *base_name = (printer_name && printer_name[0])
                            ? printer_name : dos_name;
    int name_session_id = mgr->use_session_cupsd ? 0 : mgr->session_id;
    dev->cups_name = printer_sanitize_name(base_name, name_session_id);

    mgr->printers[mgr->printer_count] = dev;
    mgr->printer_count++;

    fprintf(stderr, LOG_PREFIX "Added printer device_id=0x%x dos='%s' "
            "name='%s' driver='%s' cups='%s'\n",
            device_id, dev->dos_name,
            dev->printer_name ? dev->printer_name : "(null)",
            dev->driver_name ? dev->driver_name : "(null)",
            dev->cups_name ? dev->cups_name : "(null)");

    return dev;
}

/**
 * Find a printer device by device_id
 */
struct printer_dev *printer_dev_find(struct printer_manager *mgr,
                                     uint32_t device_id)
{
    if (mgr == NULL)
    {
        return NULL;
    }

    for (int i = 0; i < mgr->printer_count; i++)
    {
        if (mgr->printers[i] != NULL &&
            mgr->printers[i]->device_id == device_id)
        {
            return mgr->printers[i];
        }
    }

    return NULL;
}

/**
 * Free a single printer device and all its jobs
 */
static void printer_dev_free(struct printer_dev *dev)
{
    if (dev == NULL)
    {
        return;
    }

    /* Free all active jobs */
    for (int i = 0; i < PRINTER_MAX_JOBS; i++)
    {
        if (dev->jobs[i] != NULL)
        {
            printer_job_free(dev->jobs[i]);
            dev->jobs[i] = NULL;
        }
    }

    free(dev->printer_name);
    free(dev->driver_name);
    free(dev->cups_name);
    free(dev);
}

/**
 * Remove a printer device by device_id
 */
void printer_dev_remove(struct printer_manager *mgr, uint32_t device_id)
{
    if (mgr == NULL)
    {
        return;
    }

    for (int i = 0; i < mgr->printer_count; i++)
    {
        if (mgr->printers[i] != NULL &&
            mgr->printers[i]->device_id == device_id)
        {
            fprintf(stderr, LOG_PREFIX "Removing printer device_id=0x%x "
                    "name='%s'\n", device_id,
                    mgr->printers[i]->printer_name ?
                    mgr->printers[i]->printer_name : "(null)");

            printer_dev_free(mgr->printers[i]);

            /* Shift remaining entries */
            for (int j = i; j < mgr->printer_count - 1; j++)
            {
                mgr->printers[j] = mgr->printers[j + 1];
            }
            mgr->printers[mgr->printer_count - 1] = NULL;
            mgr->printer_count--;
            return;
        }
    }
}

/**
 * Remove all printer devices
 */
void printer_dev_remove_all(struct printer_manager *mgr)
{
    if (mgr == NULL)
    {
        return;
    }

    for (int i = 0; i < mgr->printer_count; i++)
    {
        printer_dev_free(mgr->printers[i]);
        mgr->printers[i] = NULL;
    }
    mgr->printer_count = 0;
}

/**
 * Create a new print job for a printer device
 */
struct printer_job *printer_job_create(struct printer_dev *dev,
                                       uint32_t file_id,
                                       const char *spool_dir)
{
    if (dev == NULL || dev->job_count >= PRINTER_MAX_JOBS)
    {
        return NULL;
    }

    struct printer_job *job = calloc(1, sizeof(struct printer_job));
    if (job == NULL)
    {
        return NULL;
    }

    job->job_id = g_next_job_id++;
    job->file_id = file_id;
    job->device_id = dev->device_id;
    job->state = PRINTER_JOB_STATE_OPEN;
    job->spool_fd = -1;

    /* Create spool file */
    if (spool_dir != NULL)
    {
        size_t path_len = strlen(spool_dir) + 64;
        job->spool_path = malloc(path_len);
        if (job->spool_path == NULL)
        {
            free(job);
            return NULL;
        }
        snprintf(job->spool_path, path_len, "%s/job_%u_%u.prn",
                 spool_dir, dev->device_id, job->job_id);

        job->spool_fd = open(job->spool_path,
                             O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (job->spool_fd < 0)
        {
            fprintf(stderr, LOG_PREFIX "Failed to create spool file '%s': %s\n",
                    job->spool_path, strerror(errno));
            free(job->spool_path);
            free(job);
            return NULL;
        }
    }

    /* Add to device's job list */
    for (int i = 0; i < PRINTER_MAX_JOBS; i++)
    {
        if (dev->jobs[i] == NULL)
        {
            dev->jobs[i] = job;
            dev->job_count++;
            break;
        }
    }

    fprintf(stderr, LOG_PREFIX "Created job %u for device 0x%x file_id=%u\n",
            job->job_id, dev->device_id, file_id);

    return job;
}

/**
 * Write data to a print job's spool file
 */
int printer_job_write(struct printer_job *job,
                      const uint8_t *data, uint32_t len)
{
    if (job == NULL || data == NULL || len == 0)
    {
        return -1;
    }

    if (job->state != PRINTER_JOB_STATE_OPEN &&
        job->state != PRINTER_JOB_STATE_WRITING)
    {
        return -1;
    }

    if (job->spool_fd < 0)
    {
        return -1;
    }

    ssize_t written = write(job->spool_fd, data, len);
    if (written < 0)
    {
        fprintf(stderr, LOG_PREFIX "Write to spool failed for job %u: %s\n",
                job->job_id, strerror(errno));
        job->state = PRINTER_JOB_STATE_ERROR;
        return -1;
    }

    job->bytes_written += (uint64_t)written;
    job->state = PRINTER_JOB_STATE_WRITING;

    return 0;
}

/**
 * Close a print job (finalize the spool file)
 */
int printer_job_close(struct printer_job *job)
{
    if (job == NULL)
    {
        return -1;
    }

    if (job->spool_fd >= 0)
    {
        close(job->spool_fd);
        job->spool_fd = -1;
    }

    job->state = PRINTER_JOB_STATE_CLOSED;

    fprintf(stderr, LOG_PREFIX "Closed job %u, %lu bytes written\n",
            job->job_id, (unsigned long)job->bytes_written);

    return 0;
}

/**
 * Free a print job and remove its spool file
 */
void printer_job_free(struct printer_job *job)
{
    if (job == NULL)
    {
        return;
    }

    if (job->spool_fd >= 0)
    {
        close(job->spool_fd);
    }

    if (job->spool_path != NULL)
    {
        unlink(job->spool_path);
        free(job->spool_path);
    }

    free(job);
}

/**
 * Find a print job by file_id within a printer device
 */
struct printer_job *printer_job_find_by_file_id(struct printer_dev *dev,
                                                 uint32_t file_id)
{
    if (dev == NULL)
    {
        return NULL;
    }

    for (int i = 0; i < PRINTER_MAX_JOBS; i++)
    {
        if (dev->jobs[i] != NULL && dev->jobs[i]->file_id == file_id)
        {
            return dev->jobs[i];
        }
    }

    return NULL;
}
