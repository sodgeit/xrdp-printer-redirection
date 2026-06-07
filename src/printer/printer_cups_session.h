/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Per-session CUPS instance management.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef PRINTER_CUPS_SESSION_H
#define PRINTER_CUPS_SESSION_H

/**
 * Start a per-session cupsd instance.
 * Creates isolated CUPS environment at /run/xrdp/cups/<display>/
 * Sets CUPS_SERVER in the process environment.
 *
 * @param session_id  X display number for this session
 * @return 0 on success, -1 on failure
 */
int printer_cups_session_start(int session_id);

/**
 * Stop the per-session cupsd and remove all state.
 */
void printer_cups_session_stop(void);

/**
 * Get the socket path of the per-session CUPS instance.
 * @return socket path, or NULL if not active
 */
const char *printer_cups_session_get_socket(void);

#endif /* PRINTER_CUPS_SESSION_H */
