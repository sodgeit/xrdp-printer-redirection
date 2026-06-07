/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - devredir extensions implementation
 *
 * This integrates with the existing xrdp chansrv/devredir infrastructure
 * to handle printer device redirection via the RDPDR channel.
 *
 * Protocol flow for sending a print job to the client:
 *   1. Server sends DR_CREATE_REQ (IRP_MJ_CREATE) to open printer port
 *   2. Client responds with DR_CREATE_RSP (file_id)
 *   3. Server sends DR_WRITE (IRP_MJ_WRITE) with print data chunks (64KB)
 *   4. Client responds with DR_WRITE_RSP for each chunk
 *   5. Server sends DR_CLOSE_REQ (IRP_MJ_CLOSE)
 *   6. Client responds with DR_CLOSE_RSP
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

/*
 * This file is designed to be compiled as part of the xrdp chansrv build.
 * It uses xrdp internal APIs: xstream_*, send_channel_data(),
 * devredir_insert_DeviceIoRequest(), IRP management, etc.
 *
 * When building standalone (for testing), define STANDALONE_BUILD
 * to use stub implementations.
 */

#ifdef STANDALONE_BUILD
/* Standalone stubs for compilation testing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
typedef unsigned int tui32;
typedef unsigned long long tui64;
typedef unsigned short tui16;
typedef unsigned char tui8;
typedef intptr_t tbus;
#define LOG(level, ...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#define LOG_DEVEL(level, ...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#define LOG_LEVEL_INFO 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_ERROR 2
#define STATUS_SUCCESS 0x00000000
static tui32 g_completion_id __attribute__((unused)) = 1000;
static int g_rdpdr_chan_id __attribute__((unused)) = 0;
#else
/* Real xrdp includes */
#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include "arch.h"
#include "parse.h"
#include "os_calls.h"
#include "string_calls.h"
#include "log.h"
#include "chansrv.h"
#include "chansrv_config.h"
#include "devredir.h"
#include "ms-rdpefs.h"
#include "irp.h"
#endif

#include "devredir_printer.h"
#include "printer/printer.h"
#include "printer/printer_cups.h"
#include "printer/printer_cups_session.h"
#include "printer/printer_socket.h"

#define LOG_PREFIX "DEVREDIR_PRN: "

/* Completion types for printer IRPs */
#define CID_PRINTER_CREATE  20
#define CID_PRINTER_WRITE   21
#define CID_PRINTER_CLOSE   22

/* Desired access for printer CREATE */
#define DA_GENERIC_WRITE    0x40000000

/* Create disposition */
#define CD_FILE_OPEN        0x00000001

/* Print send state machine */
enum print_send_state
{
    SEND_STATE_IDLE = 0,
    SEND_STATE_WAIT_CREATE,
    SEND_STATE_WRITING,
    SEND_STATE_WAIT_WRITE,
    SEND_STATE_WAIT_CLOSE,
    SEND_STATE_DONE,
    SEND_STATE_ERROR
};

/* Context for an in-progress send-to-client operation */
struct print_send_ctx
{
    tui32 device_id;
    tui32 file_id;
    tui32 completion_id;
    enum print_send_state state;
    tui8 *data;
    tui32 data_len;
    tui32 data_offset;
    struct print_send_ctx *next;
};

/* Forward declarations */
static void remove_send_ctx(struct print_send_ctx *target);
static int send_printer_create_request(struct print_send_ctx *ctx);
static int send_printer_write_request(struct print_send_ctx *ctx);
static int send_printer_close_request(struct print_send_ctx *ctx);

/* Module state */
static struct printer_manager g_printer_mgr;
static int g_initialized = 0;
static struct print_send_ctx *g_send_list = NULL;
static int g_listener_fd = -1;

/*
 * Async setup via helper child process.
 * All blocking operations (fork cupsd, system("lpadmin"), etc.) run in a
 * child process. The main event loop monitors a pipe for completion,
 * ensuring it NEVER blocks.
 *
 * States:
 *   SETUP_IDLE      - No setup needed or not yet triggered
 *   SETUP_WAITING   - Helper child running, pipe fd in select set
 *   SETUP_DONE      - Helper completed, listener can be created
 */
enum setup_state
{
    SETUP_IDLE = 0,
    SETUP_WAITING,
    SETUP_DONE
};

static enum setup_state g_setup_state = SETUP_IDLE;
static int g_setup_pipe_fd = -1;    /* Read end of pipe from helper child */
static pid_t g_setup_child_pid = -1;
static int g_setup_triggered = 0;   /* Have we started the helper? */

#ifdef STANDALONE_BUILD
/* These are defined in devredir.c when built as part of xrdp */
#else
extern tui32 g_completion_id;
extern int g_rdpdr_chan_id;
extern struct config_chansrv *g_cfg;
#endif

/* Runtime config: whether to use per-session cupsd */
static int g_use_session_cupsd = 1;

/**
 * Initialize printer redirection subsystem
 *
 * This does only non-blocking setup. All blocking work (cupsd startup,
 * lpadmin calls) runs in a helper child process to avoid stalling
 * the event loop.
 */
int devredir_printer_init(int session_id)
{
    if (g_initialized)
    {
        return 0;
    }

#ifndef STANDALONE_BUILD
    /* Check configuration -- if printer redirection is disabled, bail out */
    if (g_cfg != NULL && !g_cfg->enable_printer_redir)
    {
        LOG(LOG_LEVEL_INFO, LOG_PREFIX
            "Printer redirection disabled in sesman.ini");
        return 0;
    }

    /* Determine whether to use per-session cupsd or system cupsd */
    if (g_cfg != NULL)
    {
        g_use_session_cupsd = g_cfg->enable_per_session_cupsd;
    }
#endif

    if (printer_manager_init(&g_printer_mgr, session_id) != 0)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to initialize printer manager");
        return -1;
    }

    /* Propagate mode to printer manager for queue naming */
    g_printer_mgr.use_session_cupsd = g_use_session_cupsd;

    /*
     * If per-session cupsd mode, create the state directory now.
     * The helper child will start cupsd here shortly after.
     * /etc/profile.d/xrdp-cups-env.sh reads sesman.ini to decide whether
     * to set CUPS_SERVER, so this is no longer race-critical.
     */
    if (g_use_session_cupsd)
    {
        char base_dir[256];

        snprintf(base_dir, sizeof(base_dir),
                 "/tmp/xrdp-cups-%d-%d", (int)getuid(), session_id);
        if (mkdir(base_dir, 0700) != 0 && errno != EEXIST)
        {
            LOG(LOG_LEVEL_WARNING, LOG_PREFIX
                "Failed to pre-create state dir '%s': %s",
                base_dir, strerror(errno));
        }
    }

    /* Initialize socket handler state (no I/O, just zeroes memory) */
    printer_socket_init();

    g_initialized = 1;
    g_setup_state = SETUP_IDLE;
    g_setup_triggered = 0;

    LOG(LOG_LEVEL_INFO, LOG_PREFIX "Initialized for session %d "
        "(async setup pending, per_session_cupsd=%d)",
        session_id, g_use_session_cupsd);
    return 0;
}

/**
 * Cleanup printer redirection subsystem
 */
void devredir_printer_cleanup(void)
{
    if (!g_initialized)
    {
        return;
    }

    /* Clean up setup helper if still running */
    if (g_setup_pipe_fd >= 0)
    {
        close(g_setup_pipe_fd);
        g_setup_pipe_fd = -1;
    }
    if (g_setup_child_pid > 0)
    {
        waitpid(g_setup_child_pid, NULL, WNOHANG);
        g_setup_child_pid = -1;
    }

    /* Close listener */
    if (g_listener_fd >= 0)
    {
        printer_cups_close_listener(g_listener_fd);
        g_listener_fd = -1;
    }

    /* Close all backend connections */
    printer_socket_cleanup();

    /* Remove all CUPS queues */
    for (int i = 0; i < g_printer_mgr.printer_count; i++)
    {
        struct printer_dev *dev = g_printer_mgr.printers[i];
        if (dev != NULL && dev->cups_queue_created)
        {
            printer_cups_remove_queue(dev->cups_name);
        }
    }

    /*
     * Verification sweep: CUPS may fail to remove a queue when multiple
     * lpadmin -x commands are issued in rapid succession (scheduler race).
     * Re-scan lpstat and remove any stragglers matching our session prefix.
     */
    printer_cups_cleanup_stale_queues(
        g_use_session_cupsd ? 0 : g_printer_mgr.session_id);

    /* Free all send contexts */
    struct print_send_ctx *ctx = g_send_list;
    while (ctx != NULL)
    {
        struct print_send_ctx *next = ctx->next;
        free(ctx->data);
        free(ctx);
        ctx = next;
    }
    g_send_list = NULL;

    printer_manager_cleanup(&g_printer_mgr);

    /* Stop the per-session CUPS instance (kills cupsd, removes state) */
    if (g_use_session_cupsd)
    {
        printer_cups_session_stop();
    }

    g_initialized = 0;

    LOG(LOG_LEVEL_INFO, LOG_PREFIX "Cleanup complete");
}

/**
 * Parse printer device data from DEVICE_ANNOUNCE.
 *
 * Per MS-RDPEFS 2.2.3.1, the DeviceData for a printer contains:
 *   Flags (4 bytes)
 *   CodePage (4 bytes)
 *   PnPNameLen (4 bytes)
 *   DriverNameLen (4 bytes)
 *   PrinterNameLen (4 bytes)
 *   CachedFieldsLen (4 bytes)
 *   PnPName (variable, Unicode LE)
 *   DriverName (variable, Unicode LE)
 *   PrinterName (variable, Unicode LE)
 *   CachedPrinterConfigData (variable)
 */
int devredir_printer_device_announce(uint32_t device_id,
                                     const char *dos_name,
                                     const uint8_t *device_data,
                                     uint32_t device_data_len)
{
    if (!g_initialized)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Not initialized");
        return -1;
    }

    if (device_data == NULL || device_data_len < 24)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Invalid device data (len=%u)",
            device_data_len);
        return -1;
    }

    /* Parse the header fields (all little-endian) */
    const uint8_t *p = device_data;

    uint32_t flags = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    /* uint32_t code_page - reserved, skip */
    p += 4;

    uint32_t pnp_name_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    uint32_t driver_name_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    uint32_t printer_name_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    uint32_t cached_fields_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    /* Validate total length */
    uint32_t expected_len = 24 + pnp_name_len + driver_name_len +
                            printer_name_len + cached_fields_len;
    if (device_data_len < expected_len)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX
            "Device data too short: have %u need %u",
            device_data_len, expected_len);
        return -1;
    }

    /* Skip PnP name */
    p += pnp_name_len;

    /* Extract driver name (Unicode LE to UTF-8 basic conversion) */
    char driver_name_utf8[PRINTER_MAX_DRIVER_LEN] = {0};
    if (driver_name_len >= 2)
    {
        uint32_t j = 0;
        for (uint32_t i = 0;
             i + 1 < driver_name_len && j < sizeof(driver_name_utf8) - 1;
             i += 2)
        {
            uint8_t lo = p[i];
            uint8_t hi = p[i + 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7f)
            {
                driver_name_utf8[j++] = (char)lo;
            }
            else if (hi == 0 && lo == 0)
            {
                break; /* null terminator */
            }
        }
        driver_name_utf8[j] = '\0';
    }
    p += driver_name_len;

    /* Extract printer name (Unicode LE to UTF-8 basic conversion) */
    char printer_name_utf8[PRINTER_MAX_NAME_LEN] = {0};
    if (printer_name_len >= 2)
    {
        uint32_t j = 0;
        for (uint32_t i = 0;
             i + 1 < printer_name_len && j < sizeof(printer_name_utf8) - 1;
             i += 2)
        {
            uint8_t lo = p[i];
            uint8_t hi = p[i + 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7f)
            {
                printer_name_utf8[j++] = (char)lo;
            }
            else if (hi == 0 && lo == 0)
            {
                break; /* null terminator */
            }
        }
        printer_name_utf8[j] = '\0';
    }

    LOG(LOG_LEVEL_INFO, LOG_PREFIX
        "Printer announced: device_id=0x%x flags=0x%x "
        "driver='%s' name='%s'",
        device_id, flags, driver_name_utf8, printer_name_utf8);

    /* Add to printer manager */
    struct printer_dev *dev = printer_dev_add(&g_printer_mgr, device_id,
                                              dos_name, flags,
                                              printer_name_utf8,
                                              driver_name_utf8);
    if (dev == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to add printer device");
        return -1;
    }

    /* Queue creation happens in the async helper child.
     * Mark as pending -- the helper will create all queues. */
    dev->cups_queue_created = 0;

    /*
     * If the helper hasn't been triggered yet, that's fine -- it will
     * pick up all accumulated printers when it starts.
     * If the helper already finished (e.g., hot-plugged printer),
     * trigger a new helper run.
     */
    if (g_setup_state == SETUP_DONE && g_setup_triggered)
    {
        /* Late arrival: create queue directly in check_wait_objs via
         * a new helper. Reset state so next check_wait_objs triggers it. */
        g_setup_triggered = 0;
        g_setup_state = SETUP_IDLE;
    }

    return 0;
}

/**
 * Handle printer device removal
 */
void devredir_printer_device_remove(uint32_t device_id)
{
    if (!g_initialized)
    {
        return;
    }

    struct printer_dev *dev = printer_dev_find(&g_printer_mgr, device_id);
    if (dev == NULL)
    {
        return;
    }

    if (dev->cups_queue_created && dev->cups_name != NULL)
    {
        printer_cups_remove_queue(dev->cups_name);
    }

    printer_dev_remove(&g_printer_mgr, device_id);
}

/*
 * =========================================================================
 * IRP sending functions - these use the xrdp stream API to send packets
 * to the client via the RDPDR virtual channel.
 * =========================================================================
 */

/**
 * Send IRP_MJ_CREATE to open the printer port on the client.
 *
 * For printers, the path is empty (PathLength=0) since the device
 * itself IS the printer port.
 */
static int send_printer_create_request(struct print_send_ctx *ctx)
{
#ifndef STANDALONE_BUILD
    struct stream *s;
    int bytes;
    IRP *irp;

    irp = devredir_irp_new();
    if (irp == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to allocate IRP for CREATE");
        return -1;
    }

    irp->CompletionId = g_completion_id++;
    irp->DeviceId = ctx->device_id;
    irp->completion_type = CID_PRINTER_CREATE;
    irp->callback = devredir_printer_irp_callback;
    irp->user_data = ctx;

    ctx->completion_id = irp->CompletionId;

    xstream_new(s, 128);

    devredir_insert_DeviceIoRequest(s,
                                    ctx->device_id,
                                    0,              /* FileId = 0 for create */
                                    irp->CompletionId,
                                    IRP_MJ_CREATE,
                                    IRP_MN_NONE);

    xstream_wr_u32_le(s, DA_GENERIC_WRITE);  /* DesiredAccess       */
    xstream_wr_u32_le(s, 0);                 /* AllocationSize high */
    xstream_wr_u32_le(s, 0);                 /* AllocationSize low  */
    xstream_wr_u32_le(s, 0);                 /* FileAttributes      */
    xstream_wr_u32_le(s, 0);                 /* SharedAccess = none */
    xstream_wr_u32_le(s, CD_FILE_OPEN);      /* CreateDisposition   */
    xstream_wr_u32_le(s, 0);                 /* CreateOptions       */
    xstream_wr_u32_le(s, 0);                 /* PathLength = 0      */

    bytes = xstream_len(s);
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);

    LOG_DEVEL(LOG_LEVEL_DEBUG, LOG_PREFIX
              "Sent CREATE for device 0x%x CompletionId=%u",
              ctx->device_id, irp->CompletionId);
#else
    ctx->completion_id = g_completion_id++;
    LOG(LOG_LEVEL_DEBUG, LOG_PREFIX
        "Sent CREATE for device 0x%x CompletionId=%u",
        ctx->device_id, ctx->completion_id);
#endif

    ctx->state = SEND_STATE_WAIT_CREATE;
    return 0;
}

/**
 * Send IRP_MJ_WRITE with a chunk of print data (max 64KB).
 */
static int send_printer_write_request(struct print_send_ctx *ctx)
{
    tui32 chunk_size = ctx->data_len - ctx->data_offset;
    if (chunk_size > PRINTER_WRITE_CHUNK_SIZE)
    {
        chunk_size = PRINTER_WRITE_CHUNK_SIZE;
    }

#ifndef STANDALONE_BUILD
    struct stream *s;
    int bytes;
    IRP *irp;

    irp = devredir_irp_new();
    if (irp == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to allocate IRP for WRITE");
        return -1;
    }

    irp->CompletionId = g_completion_id++;
    irp->DeviceId = ctx->device_id;
    irp->FileId = ctx->file_id;
    irp->completion_type = CID_PRINTER_WRITE;
    irp->callback = devredir_printer_irp_callback;
    irp->user_data = ctx;

    ctx->completion_id = irp->CompletionId;

    xstream_new(s, 64 + chunk_size);

    devredir_insert_DeviceIoRequest(s,
                                    ctx->device_id,
                                    ctx->file_id,
                                    irp->CompletionId,
                                    IRP_MJ_WRITE,
                                    IRP_MN_NONE);

    xstream_wr_u32_le(s, chunk_size);                          /* Length  */
    xstream_wr_u64_le(s, (tui64)ctx->data_offset);            /* Offset  */
    xstream_seek(s, 20);                                       /* Padding */
    xstream_copyin(s, ctx->data + ctx->data_offset, chunk_size); /* Data  */

    bytes = xstream_len(s);
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);

    LOG_DEVEL(LOG_LEVEL_DEBUG, LOG_PREFIX
              "Sent WRITE device=0x%x file_id=%u offset=%u len=%u",
              ctx->device_id, ctx->file_id, ctx->data_offset, chunk_size);
#else
    ctx->completion_id = g_completion_id++;
    LOG(LOG_LEVEL_DEBUG, LOG_PREFIX
        "Sent WRITE device=0x%x file_id=%u offset=%u len=%u",
        ctx->device_id, ctx->file_id, ctx->data_offset, chunk_size);
#endif

    ctx->state = SEND_STATE_WAIT_WRITE;
    return 0;
}

/**
 * Send IRP_MJ_CLOSE to close the printer port.
 */
static int send_printer_close_request(struct print_send_ctx *ctx)
{
#ifndef STANDALONE_BUILD
    struct stream *s;
    int bytes;
    IRP *irp;

    irp = devredir_irp_new();
    if (irp == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to allocate IRP for CLOSE");
        return -1;
    }

    irp->CompletionId = g_completion_id++;
    irp->DeviceId = ctx->device_id;
    irp->FileId = ctx->file_id;
    irp->completion_type = CID_PRINTER_CLOSE;
    irp->callback = devredir_printer_irp_callback;
    irp->user_data = ctx;

    ctx->completion_id = irp->CompletionId;

    xstream_new(s, 128);

    devredir_insert_DeviceIoRequest(s,
                                    ctx->device_id,
                                    ctx->file_id,
                                    irp->CompletionId,
                                    IRP_MJ_CLOSE,
                                    IRP_MN_NONE);

    xstream_seek(s, 32); /* Padding per DR_CLOSE_REQ */

    bytes = xstream_len(s);
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);

    LOG_DEVEL(LOG_LEVEL_DEBUG, LOG_PREFIX
              "Sent CLOSE for device=0x%x file_id=%u",
              ctx->device_id, ctx->file_id);
#else
    ctx->completion_id = g_completion_id++;
    LOG(LOG_LEVEL_DEBUG, LOG_PREFIX
        "Sent CLOSE for device=0x%x file_id=%u",
        ctx->device_id, ctx->file_id);
#endif

    ctx->state = SEND_STATE_WAIT_CLOSE;
    return 0;
}

/*
 * =========================================================================
 * IRP Callback - handles all printer IRP completions via irp->callback
 * =========================================================================
 */
#ifndef STANDALONE_BUILD
/**
 * Unified IRP callback for printer operations.
 * This is set as irp->callback and dispatches CREATE/WRITE/CLOSE responses.
 */
void devredir_printer_irp_callback(struct stream *s, IRP *irp,
                                   tui32 DeviceId, tui32 CompletionId,
                                   tui32 IoStatus)
{
    struct print_send_ctx *ctx = (struct print_send_ctx *)irp->user_data;

    if (ctx == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX
            "IRP callback with NULL context, CompletionId=%u", CompletionId);
        devredir_irp_delete(irp);
        return;
    }

    switch (irp->completion_type)
    {
        case CID_PRINTER_CREATE:
        {
            if (IoStatus != STATUS_SUCCESS)
            {
                LOG(LOG_LEVEL_ERROR, LOG_PREFIX
                    "CREATE failed device=0x%x status=0x%x",
                    DeviceId, IoStatus);
                ctx->state = SEND_STATE_ERROR;
                devredir_irp_delete(irp);
                remove_send_ctx(ctx);
                return;
            }

            /* Read the FileId from DR_CREATE_RSP */
            if (!s_check_rem_and_log(s, 4, "Parsing DR_CREATE_RSP"))
            {
                ctx->state = SEND_STATE_ERROR;
                devredir_irp_delete(irp);
                remove_send_ctx(ctx);
                return;
            }
            tui32 file_id;
            xstream_rd_u32_le(s, file_id);
            ctx->file_id = file_id;

            LOG_DEVEL(LOG_LEVEL_DEBUG, LOG_PREFIX
                      "CREATE success device=0x%x file_id=%u",
                      DeviceId, file_id);

            devredir_irp_delete(irp);

            /* Begin writing data */
            send_printer_write_request(ctx);
            break;
        }

        case CID_PRINTER_WRITE:
        {
            if (IoStatus != STATUS_SUCCESS)
            {
                LOG(LOG_LEVEL_ERROR, LOG_PREFIX
                    "WRITE failed device=0x%x status=0x%x",
                    DeviceId, IoStatus);
                ctx->state = SEND_STATE_ERROR;
                devredir_irp_delete(irp);
                /* Still try to close */
                send_printer_close_request(ctx);
                return;
            }

            /* Read bytes written from DR_WRITE_RSP */
            if (!s_check_rem_and_log(s, 4, "Parsing DR_WRITE_RSP"))
            {
                ctx->state = SEND_STATE_ERROR;
                devredir_irp_delete(irp);
                send_printer_close_request(ctx);
                return;
            }
            tui32 bytes_written;
            xstream_rd_u32_le(s, bytes_written);
            ctx->data_offset += bytes_written;

            LOG_DEVEL(LOG_LEVEL_DEBUG, LOG_PREFIX
                      "WRITE response: %u bytes, total %u/%u",
                      bytes_written, ctx->data_offset, ctx->data_len);

            devredir_irp_delete(irp);

            if (ctx->data_offset >= ctx->data_len)
            {
                /* All data sent, close the printer port */
                send_printer_close_request(ctx);
            }
            else
            {
                /* Send next chunk */
                send_printer_write_request(ctx);
            }
            break;
        }

        case CID_PRINTER_CLOSE:
        {
            if (IoStatus != STATUS_SUCCESS)
            {
                LOG(LOG_LEVEL_ERROR, LOG_PREFIX
                    "CLOSE failed device=0x%x status=0x%x",
                    DeviceId, IoStatus);
            }
            else
            {
                LOG(LOG_LEVEL_INFO, LOG_PREFIX
                    "Print job complete: device=0x%x (%u bytes sent)",
                    DeviceId, ctx->data_offset);
            }
            ctx->state = SEND_STATE_DONE;
            devredir_irp_delete(irp);
            remove_send_ctx(ctx);
            break;
        }

        default:
            LOG(LOG_LEVEL_ERROR, LOG_PREFIX
                "Unknown completion_type %d", irp->completion_type);
            devredir_irp_delete(irp);
            break;
    }
}
#endif /* !STANDALONE_BUILD */

/*
 * =========================================================================
 * Send context management
 * =========================================================================
 */

static void remove_send_ctx(struct print_send_ctx *target)
{
    if (target == NULL)
    {
        return;
    }

    if (g_send_list == target)
    {
        g_send_list = target->next;
    }
    else
    {
        struct print_send_ctx *ctx = g_send_list;
        while (ctx != NULL && ctx->next != target)
        {
            ctx = ctx->next;
        }
        if (ctx != NULL)
        {
            ctx->next = target->next;
        }
    }

    free(target->data);
    free(target);
}

/**
 * Send a print job to the client printer.
 * Initiates the IRP CREATE -> WRITE -> CLOSE sequence.
 */
int devredir_printer_send_job(uint32_t device_id,
                              const uint8_t *data,
                              uint32_t data_len)
{
    if (!g_initialized || data == NULL || data_len == 0)
    {
        return -1;
    }

    struct printer_dev *dev = printer_dev_find(&g_printer_mgr, device_id);
    if (dev == NULL)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX
            "send_job: device 0x%x not found", device_id);
        return -1;
    }

    /* Create a send context */
    struct print_send_ctx *ctx = calloc(1, sizeof(struct print_send_ctx));
    if (ctx == NULL)
    {
        return -1;
    }

    ctx->device_id = device_id;
    ctx->state = SEND_STATE_IDLE;
    ctx->data_len = data_len;
    ctx->data_offset = 0;

    /* Copy the print data */
    ctx->data = malloc(data_len);
    if (ctx->data == NULL)
    {
        free(ctx);
        return -1;
    }
    memcpy(ctx->data, data, data_len);

    /* Add to send list */
    ctx->next = g_send_list;
    g_send_list = ctx;

    LOG(LOG_LEVEL_INFO, LOG_PREFIX
        "Starting print job: device=0x%x size=%u bytes",
        device_id, data_len);

    /* Start the IRP sequence with CREATE */
    return send_printer_create_request(ctx);
}

/**
 * Check if a device_id is a printer
 */
int devredir_printer_is_printer_device(uint32_t device_id)
{
    if (!g_initialized)
    {
        return 0;
    }
    return (printer_dev_find(&g_printer_mgr, device_id) != NULL) ? 1 : 0;
}

/**
 * Get the printer manager instance
 */
struct printer_manager *devredir_printer_get_manager(void)
{
    return &g_printer_mgr;
}

/*
 * =========================================================================
 * Wait objects / event loop integration for chansrv
 * =========================================================================
 */

/**
 * Get wait objects for the printer subsystem.
 * Called from devredir_get_wait_objs() to add printer listener to poll set.
 */
int devredir_printer_get_wait_objs(tbus *objs, int *count, int *timeout)
{
    if (!g_initialized)
    {
        return 0;
    }

    /* Add the setup pipe fd if we're waiting for the helper child */
    if (g_setup_state == SETUP_WAITING && g_setup_pipe_fd >= 0)
    {
        objs[*count] = (tbus)g_setup_pipe_fd;
        (*count)++;
    }

    /* Add the listener socket if available */
    if (g_listener_fd >= 0)
    {
        objs[*count] = (tbus)g_listener_fd;
        (*count)++;

        /* Add all active backend connection fds */
        int fds[MAX_BACKEND_CONNECTIONS];
        int nfds = printer_socket_get_fds(fds, MAX_BACKEND_CONNECTIONS);
        for (int i = 0; i < nfds; i++)
        {
            objs[*count] = (tbus)fds[i];
            (*count)++;
        }
    }

    return 0;
}

/**
 * Launch the async helper child that does all blocking setup work.
 * The child:
 *   1. Starts the per-session cupsd (fork + wait for socket)
 *   2. Creates CUPS queues for all announced printers (lpadmin calls)
 *   3. Writes result to pipe and exits
 *
 * The parent (event loop) monitors the pipe read-end via select().
 */
static void launch_setup_helper(void)
{
    int pipefd[2];

    if (pipe(pipefd) != 0)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "pipe() failed: %s", strerror(errno));
        return;
    }

    /* Set read end non-blocking + close-on-exec */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0)
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX "fork() helper failed: %s",
            strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0)
    {
        /* === Child process: do all blocking work === */
        close(pipefd[0]); /* Close read end */

        /*
         * Reset all signal handlers to default. xrdp's chansrv may have
         * custom SIGCHLD/SIGPIPE handlers that interfere with system()
         * and fork()/waitpid() in the child.
         */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        /* Safety timeout: kill ourselves if stuck for more than 30s */
        alarm(30);

        /* Close all inherited fds except our pipe write end and stdio */
        int maxfd = (int)sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
        {
            maxfd = 4096;
        }
        for (int fd = 3; fd < maxfd; fd++)
        {
            if (fd != pipefd[1])
            {
                close(fd);
            }
        }

        int session_id = g_printer_mgr.session_id;
        char result = 'F'; /* F=fail, S=success */

        if (g_use_session_cupsd)
        {
            /* === Per-session cupsd mode === */
            fprintf(stderr, LOG_PREFIX "HELPER[%d]: starting cupsd for "
                    "session %d\n", (int)getpid(), session_id);

            /* Start per-session cupsd */
            if (printer_cups_session_start(session_id) == 0)
            {
                fprintf(stderr, LOG_PREFIX "HELPER[%d]: cupsd started, "
                        "creating %d queues\n",
                        (int)getpid(), g_printer_mgr.printer_count);

                /* Create queues for all announced printers */
                for (int i = 0; i < g_printer_mgr.printer_count; i++)
                {
                    struct printer_dev *dev = g_printer_mgr.printers[i];
                    if (dev == NULL || dev->cups_name == NULL)
                    {
                        continue;
                    }

                    fprintf(stderr, LOG_PREFIX "HELPER[%d]: creating "
                            "queue '%s'\n", (int)getpid(), dev->cups_name);

                    if (printer_cups_create_queue(dev->cups_name,
                                                  session_id,
                                                  dev->device_id) == 0)
                    {
                        if (dev->flags &
                            RDPDR_PRINTER_ANNOUNCE_FLAG_DEFAULTPRINTER)
                        {
                            printer_cups_set_default(dev->cups_name);
                        }
                    }
                }
                result = 'S';
                fprintf(stderr, LOG_PREFIX "HELPER[%d]: all queues done\n",
                        (int)getpid());

                /* Forward system printers into our instance */
                fprintf(stderr, LOG_PREFIX "HELPER[%d]: forwarding system "
                        "printers\n", (int)getpid());
                {
                    char fwd_cmd[512];
                    const char *cups_sock =
                        printer_cups_session_get_socket();
                    snprintf(fwd_cmd, sizeof(fwd_cmd),
                             "CUPS_SERVER='%s' "
                             "/usr/share/xrdp/xrdp-cups-forward-printers "
                             "/run/cups/cups.sock %d 2>/dev/null",
                             cups_sock ? cups_sock : "", session_id);
                    int fwd_ret = system(fwd_cmd);
                    fprintf(stderr, LOG_PREFIX "HELPER[%d]: forward "
                            "returned %d\n", (int)getpid(), fwd_ret);
                }
            }
            else
            {
                fprintf(stderr, LOG_PREFIX "HELPER[%d]: cupsd start "
                        "FAILED\n", (int)getpid());
            }
        }
        else
        {
            /* === System cupsd mode === */
            fprintf(stderr, LOG_PREFIX "HELPER[%d]: using system cupsd, "
                    "creating %d queues\n",
                    (int)getpid(), g_printer_mgr.printer_count);

            for (int i = 0; i < g_printer_mgr.printer_count; i++)
            {
                struct printer_dev *dev = g_printer_mgr.printers[i];
                if (dev == NULL || dev->cups_name == NULL)
                {
                    continue;
                }

                fprintf(stderr, LOG_PREFIX "HELPER[%d]: creating queue "
                        "'%s' on system cupsd\n",
                        (int)getpid(), dev->cups_name);

                if (printer_cups_create_queue(dev->cups_name,
                                              session_id,
                                              dev->device_id) == 0)
                {
                    if (dev->flags &
                        RDPDR_PRINTER_ANNOUNCE_FLAG_DEFAULTPRINTER)
                    {
                        printer_cups_set_default(dev->cups_name);
                    }
                }
            }
            result = 'S';
            fprintf(stderr, LOG_PREFIX "HELPER[%d]: all queues done "
                    "(system cupsd)\n", (int)getpid());
        }

        /* Signal parent */
        fprintf(stderr, LOG_PREFIX "HELPER[%d]: signaling parent result='%c'\n",
                (int)getpid(), result);
        if (write(pipefd[1], &result, 1) < 0)
        {
            fprintf(stderr, LOG_PREFIX "HELPER[%d]: write to pipe failed: %s\n",
                    (int)getpid(), strerror(errno));
        }
        close(pipefd[1]);
        fprintf(stderr, LOG_PREFIX "HELPER[%d]: exiting\n", (int)getpid());
        _exit(0);
    }

    /* === Parent process === */
    close(pipefd[1]); /* Close write end */

    g_setup_pipe_fd = pipefd[0];
    g_setup_child_pid = pid;
    g_setup_state = SETUP_WAITING;
    g_setup_triggered = 1;

    LOG(LOG_LEVEL_INFO, LOG_PREFIX
        "Launched setup helper (pid=%d) for session %d",
        (int)pid, g_printer_mgr.session_id);
}

/**
 * Handle completion of the helper child.
 * Called when the pipe fd becomes readable.
 */
static void handle_setup_complete(void)
{
    char result = 'F';
    ssize_t n = read(g_setup_pipe_fd, &result, 1);

    close(g_setup_pipe_fd);
    g_setup_pipe_fd = -1;

    /* Reap the child */
    if (g_setup_child_pid > 0)
    {
        waitpid(g_setup_child_pid, NULL, 0);
        g_setup_child_pid = -1;
    }

    if (n == 1 && result == 'S')
    {
        LOG(LOG_LEVEL_INFO, LOG_PREFIX "Setup helper succeeded");

        /* Mark all printers as having queues created */
        for (int i = 0; i < g_printer_mgr.printer_count; i++)
        {
            struct printer_dev *dev = g_printer_mgr.printers[i];
            if (dev != NULL && dev->cups_name != NULL)
            {
                dev->cups_queue_created = 1;
            }
        }

        if (g_use_session_cupsd)
        {
            /*
             * Set CUPS_SERVER in parent so cleanup/remove_queue calls
             * target the private instance. The child set it in its own
             * env (lost after exit), so we replicate the path here.
             */
            char base_dir[256];
            char sock_path[280];
            int sid = g_printer_mgr.session_id;

            snprintf(base_dir, sizeof(base_dir),
                     "/tmp/xrdp-cups-%d-%d", (int)getuid(), sid);
            snprintf(sock_path, sizeof(sock_path), "%s/sock", base_dir);
            setenv("CUPS_SERVER", sock_path, 1);
            LOG(LOG_LEVEL_INFO, LOG_PREFIX "Set CUPS_SERVER=%s", sock_path);
        }

        /* Now create the listener socket in the parent */
        g_listener_fd = printer_cups_init_listener(g_printer_mgr.session_id);
        if (g_listener_fd < 0)
        {
            LOG(LOG_LEVEL_ERROR, LOG_PREFIX "Failed to create listener socket");
        }
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, LOG_PREFIX
            "Setup helper failed (result='%c')", result);
        /* Fall back to system CUPS -- clean stale queues */
        printer_cups_cleanup_stale_queues(
            g_use_session_cupsd ? 0 : g_printer_mgr.session_id);
    }

    g_setup_state = SETUP_DONE;
}

/**
 * Check wait objects for the printer subsystem.
 * Called from devredir_check_wait_objs() to handle incoming connections
 * from the CUPS backend and relay jobs to the client.
 */
int devredir_printer_check_wait_objs(void)
{
    if (!g_initialized)
    {
        return 0;
    }

    /*
     * Trigger the async helper once we have printers announced.
     * We wait until at least one printer is registered so the helper
     * can create all queues in one batch.
     */
    if (g_setup_state == SETUP_IDLE && !g_setup_triggered &&
        g_printer_mgr.printer_count > 0)
    {
        launch_setup_helper();
    }

    /* Check if helper child has finished */
    if (g_setup_state == SETUP_WAITING && g_setup_pipe_fd >= 0)
    {
        struct pollfd pfd = { .fd = g_setup_pipe_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0)
        {
            handle_setup_complete();
        }
    }

    /* Normal operation: handle backend connections */
    if (g_listener_fd >= 0)
    {
        int rc = printer_socket_accept(g_listener_fd);
        if (rc < 0)
        {
            LOG(LOG_LEVEL_ERROR, LOG_PREFIX
                "Listener socket error, disabling");
            close(g_listener_fd);
            g_listener_fd = -1;
        }

        printer_socket_check_all();
    }

    return 0;
}
