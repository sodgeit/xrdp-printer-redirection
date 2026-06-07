/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - XPS format conversion
 *
 * When a printer is marked with RDPDR_PRINTER_ANNOUNCE_FLAG_XPSFORMAT,
 * the client expects print data in XPS (XML Paper Specification) format.
 * Since most Linux applications generate PostScript or PDF, we need to
 * convert: PostScript -> PDF (via ghostscript) -> XPS (via gxps/pdftoXPS)
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

#ifndef PRINTER_XPS_H
#define PRINTER_XPS_H

#include <stdint.h>

/**
 * Check if XPS conversion tools are available on the system.
 *
 * @return 1 if available, 0 if not
 */
int printer_xps_available(void);

/**
 * Convert PostScript data to XPS format.
 * Pipeline: PS -> PDF (ghostscript) -> XPS (gxps-convert)
 *
 * @param ps_data      Input PostScript data
 * @param ps_len       Length of PostScript data
 * @param xps_data     Output: allocated buffer with XPS data (caller frees)
 * @param xps_len      Output: length of XPS data
 * @param spool_dir    Directory for temporary files
 * @return 0 on success, -1 on failure
 */
int printer_xps_convert_ps(const uint8_t *ps_data, uint32_t ps_len,
                           uint8_t **xps_data, uint32_t *xps_len,
                           const char *spool_dir);

/**
 * Convert PDF data to XPS format.
 *
 * @param pdf_data     Input PDF data
 * @param pdf_len      Length of PDF data
 * @param xps_data     Output: allocated buffer with XPS data (caller frees)
 * @param xps_len      Output: length of XPS data
 * @param spool_dir    Directory for temporary files
 * @return 0 on success, -1 on failure
 */
int printer_xps_convert_pdf(const uint8_t *pdf_data, uint32_t pdf_len,
                            uint8_t **xps_data, uint32_t *xps_len,
                            const char *spool_dir);

/**
 * Detect the format of print data.
 *
 * @param data     Print data buffer
 * @param len      Length of data
 * @return format string: "ps", "pdf", "xps", or "raw"
 */
const char *printer_detect_format(const uint8_t *data, uint32_t len);

#endif /* PRINTER_XPS_H */
