/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - devredir extensions for printer devices
 *
 * This module extends the existing devredir.c to handle RDPDR_DTYP_PRINT
 * device announcements and print job IRP sequences (CREATE/WRITE/CLOSE).
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

#ifndef DEVREDIR_PRINTER_H
#define DEVREDIR_PRINTER_H

#include <stdint.h>
#include "printer/printer.h"

#ifdef STANDALONE_BUILD
typedef intptr_t tbus;
typedef unsigned int tui32;
struct stream;
typedef struct irp IRP;
#else
/* Forward declarations for xrdp types.
 * Full definitions come from arch.h, parse.h, irp.h when included
 * by the compilation unit. These allow this header to be included
 * without requiring all xrdp headers first. */
#ifndef ARCH_H
typedef intptr_t tbus;
typedef unsigned int tui32;
#endif
#ifndef IRP_H
struct stream;
struct irp;
typedef struct irp IRP;
#endif
#endif

/**
 * Initialize printer redirection subsystem.
 * Called during chansrv/devredir initialization.
 *
 * @param session_id  The xrdp session identifier
 * @return 0 on success, -1 on failure
 */
int devredir_printer_init(int session_id);

/**
 * Cleanup printer redirection subsystem.
 * Called during chansrv/devredir shutdown.
 * Removes all CUPS queues and frees resources.
 */
void devredir_printer_cleanup(void);

/**
 * Handle a printer device announcement from the client.
 * Called from devredir when device_type == RDPDR_DTYP_PRINT.
 *
 * Parses the DEVICE_ANNOUNCE structure for printer-specific data:
 *   - Flags (RDPDR_PRINTER_ANNOUNCE_FLAG_*)
 *   - CodePage
 *   - PnPNameLen + PnPName
 *   - DriverNameLen + DriverName
 *   - PrinterNameLen + PrinterName
 *   - CachedFieldsLen + CachedPrinterConfigData
 *
 * @param device_id        The device ID assigned by the client
 * @param dos_name         The 8-char preferred DOS name
 * @param device_data      Pointer to printer-specific device data
 * @param device_data_len  Length of device data
 * @return 0 on success, -1 on failure
 */
int devredir_printer_device_announce(uint32_t device_id,
                                     const char *dos_name,
                                     const uint8_t *device_data,
                                     uint32_t device_data_len);

/**
 * Handle printer device removal.
 * Called when the client removes a printer device.
 *
 * @param device_id  The device ID being removed
 */
void devredir_printer_device_remove(uint32_t device_id);

/**
 * Handle IRP_MJ_CREATE completion for a printer device.
 * The client has acknowledged opening the printer port.
 *
 * @param device_id      Device ID
 * @param completion_id  IRP completion ID
 * @param io_status      NT status of the operation
 * @param file_id        File ID assigned by client (used for subsequent I/O)
 * @return 0 on success, -1 on failure
 */
int devredir_printer_irp_create_response(uint32_t device_id,
                                          uint32_t completion_id,
                                          uint32_t io_status,
                                          uint32_t file_id);

/**
 * Handle IRP_MJ_WRITE completion for a printer device.
 * The client has acknowledged receiving print data.
 *
 * @param device_id      Device ID
 * @param completion_id  IRP completion ID
 * @param io_status      NT status of the operation
 * @param bytes_written  Number of bytes the client accepted
 * @return 0 on success, -1 on failure
 */
int devredir_printer_irp_write_response(uint32_t device_id,
                                         uint32_t completion_id,
                                         uint32_t io_status,
                                         uint32_t bytes_written);

/**
 * Handle IRP_MJ_CLOSE completion for a printer device.
 * The client has acknowledged closing the printer port.
 *
 * @param device_id      Device ID
 * @param completion_id  IRP completion ID
 * @param io_status      NT status of the operation
 * @return 0 on success, -1 on failure
 */
int devredir_printer_irp_close_response(uint32_t device_id,
                                         uint32_t completion_id,
                                         uint32_t io_status);

/**
 * Send a print job to the client printer.
 * This initiates the IRP sequence: CREATE -> WRITE (chunked) -> CLOSE.
 *
 * @param device_id   Device ID of the target printer
 * @param data        Print job data (RAW or XPS)
 * @param data_len    Length of print data
 * @return 0 on success, -1 on failure
 */
int devredir_printer_send_job(uint32_t device_id,
                              const uint8_t *data,
                              uint32_t data_len);

/**
 * Check if a device_id corresponds to a printer device.
 *
 * @param device_id  The device ID to check
 * @return 1 if printer, 0 if not
 */
int devredir_printer_is_printer_device(uint32_t device_id);

/**
 * Get the printer manager (for use by CUPS backend communication).
 */
struct printer_manager *devredir_printer_get_manager(void);

/**
 * Get wait objects for event loop integration.
 * Add printer listener socket to the poll set.
 */
int devredir_printer_get_wait_objs(tbus *objs, int *count, int *timeout);

/**
 * Check wait objects - handle CUPS backend connections.
 */
int devredir_printer_check_wait_objs(void);

#ifndef STANDALONE_BUILD
/**
 * IRP callback for printer completions (CREATE/WRITE/CLOSE).
 * Set as irp->callback when sending printer IRPs.
 */
void devredir_printer_irp_callback(struct stream *s, IRP *irp,
                                   tui32 DeviceId, tui32 CompletionId,
                                   tui32 IoStatus);
#endif

#endif /* DEVREDIR_PRINTER_H */
