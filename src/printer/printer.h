/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - device management
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

#ifndef PRINTER_H
#define PRINTER_H

#include <stdint.h>

/* MS-RDPEFS device types (only define if not already provided by ms-rdpefs.h) */
#ifndef RDPDR_DTYP_PRINT
#define RDPDR_DTYP_SERIAL      0x00000001
#define RDPDR_DTYP_PARALLEL    0x00000002
#define RDPDR_DTYP_PRINT       0x00000004
#define RDPDR_DTYP_FILESYSTEM  0x00000008
#define RDPDR_DTYP_SMARTCARD   0x00000020
#endif

/* Printer flags (from MS-RDPEFS 2.2.3.1) */
#define RDPDR_PRINTER_ANNOUNCE_FLAG_ASCII          0x00000001
#define RDPDR_PRINTER_ANNOUNCE_FLAG_DEFAULTPRINTER 0x00000002
#define RDPDR_PRINTER_ANNOUNCE_FLAG_NETWORKPRINTER 0x00000004
#define RDPDR_PRINTER_ANNOUNCE_FLAG_TSPRINTER      0x00000008
#define RDPDR_PRINTER_ANNOUNCE_FLAG_XPSFORMAT      0x00000010

/* Print job states */
enum printer_job_state
{
    PRINTER_JOB_STATE_NONE = 0,
    PRINTER_JOB_STATE_OPEN,
    PRINTER_JOB_STATE_WRITING,
    PRINTER_JOB_STATE_CLOSED,
    PRINTER_JOB_STATE_ERROR
};

/* Maximum values */
#define PRINTER_MAX_NAME_LEN       256
#define PRINTER_MAX_DRIVER_LEN     256
#define PRINTER_MAX_PRINTERS       16
#define PRINTER_MAX_JOBS           32
#define PRINTER_WRITE_CHUNK_SIZE   65536

/* A single print job */
struct printer_job
{
    uint32_t job_id;
    uint32_t file_id;
    uint32_t device_id;
    enum printer_job_state state;
    char *spool_path;          /* path to spooled file on disk */
    int spool_fd;              /* fd for the spool file */
    uint64_t bytes_written;
};

/* A redirected printer device */
struct printer_dev
{
    uint32_t device_id;
    uint32_t flags;
    char dos_name[9];          /* 8-char DOS name + null */
    char *printer_name;        /* Unicode printer name (converted to UTF-8) */
    char *driver_name;         /* Unicode driver name (converted to UTF-8) */
    char *cups_name;           /* Sanitized CUPS queue name */
    int cups_queue_created;    /* 1 if we created the CUPS queue */
    struct printer_job *jobs[PRINTER_MAX_JOBS];
    int job_count;
};

/* Global printer manager state */
struct printer_manager
{
    struct printer_dev *printers[PRINTER_MAX_PRINTERS];
    int printer_count;
    int session_id;
    int use_session_cupsd;     /* 1=per-session cupsd (stable names), 0=system cupsd */
    char spool_dir[256];       /* Base spool directory for this session */
};

/* Initialization / teardown */
int printer_manager_init(struct printer_manager *mgr, int session_id);
void printer_manager_cleanup(struct printer_manager *mgr);

/* Printer device management */
struct printer_dev *printer_dev_add(struct printer_manager *mgr,
                                    uint32_t device_id,
                                    const char *dos_name,
                                    uint32_t flags,
                                    const char *printer_name,
                                    const char *driver_name);
void printer_dev_remove(struct printer_manager *mgr, uint32_t device_id);
struct printer_dev *printer_dev_find(struct printer_manager *mgr,
                                     uint32_t device_id);
void printer_dev_remove_all(struct printer_manager *mgr);

/* Print job management */
struct printer_job *printer_job_create(struct printer_dev *dev,
                                       uint32_t file_id,
                                       const char *spool_dir);
int printer_job_write(struct printer_job *job,
                      const uint8_t *data, uint32_t len);
int printer_job_close(struct printer_job *job);
void printer_job_free(struct printer_job *job);
struct printer_job *printer_job_find_by_file_id(struct printer_dev *dev,
                                                 uint32_t file_id);

/* Utility */
char *printer_sanitize_name(const char *name, int session_id);

#endif /* PRINTER_H */
