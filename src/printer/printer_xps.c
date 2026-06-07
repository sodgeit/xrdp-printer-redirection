/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Printer redirection - XPS format conversion implementation
 *
 * Conversion pipeline:
 *   PostScript -> PDF: ghostscript (gs -sDEVICE=pdfwrite)
 *   PDF -> XPS:        gxps-convert (from libgxps-utils) or
 *                      alternative: pdf2xps from mupdf-tools
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
#include <sys/stat.h>
#include <sys/wait.h>

#include "printer_xps.h"

#define LOG_PREFIX "PRN_XPS: "

/**
 * Check if a command exists in PATH
 */
static int command_exists(const char *cmd)
{
    char check[256];
    snprintf(check, sizeof(check), "which %s >/dev/null 2>&1", cmd);
    return (system(check) == 0) ? 1 : 0;
}

/**
 * Check if XPS conversion tools are available
 */
int printer_xps_available(void)
{
    /* Need ghostscript for PS->PDF */
    if (!command_exists("gs"))
    {
        fprintf(stderr, LOG_PREFIX "ghostscript (gs) not found\n");
        return 0;
    }

    /* Need either gxps-convert or mutool for PDF->XPS */
    if (!command_exists("gxps-convert") && !command_exists("mutool"))
    {
        fprintf(stderr, LOG_PREFIX
                "Neither gxps-convert nor mutool found for PDF->XPS\n");
        return 0;
    }

    return 1;
}

/**
 * Read an entire file into a buffer
 */
static int read_file_to_buffer(const char *path, uint8_t **data, uint32_t *len)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return -1;
    }

    if (st.st_size == 0 || st.st_size > 512 * 1024 * 1024) /* 512MB limit */
    {
        return -1;
    }

    *data = malloc((size_t)st.st_size);
    if (*data == NULL)
    {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        free(*data);
        *data = NULL;
        return -1;
    }

    size_t remaining = (size_t)st.st_size;
    uint8_t *p = *data;
    while (remaining > 0)
    {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0)
        {
            close(fd);
            free(*data);
            *data = NULL;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }

    close(fd);
    *len = (uint32_t)st.st_size;
    return 0;
}

/**
 * Write buffer to a file
 */
static int write_buffer_to_file(const char *path, const uint8_t *data,
                                uint32_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return -1;
    }

    const uint8_t *p = data;
    uint32_t remaining = len;
    while (remaining > 0)
    {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0)
        {
            close(fd);
            return -1;
        }
        p += n;
        remaining -= (uint32_t)n;
    }

    close(fd);
    return 0;
}

/**
 * Convert PostScript to PDF using ghostscript
 */
static int ps_to_pdf(const char *ps_path, const char *pdf_path)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd),
             "gs -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite "
             "-sOutputFile='%s' '%s' >/dev/null 2>&1",
             pdf_path, ps_path);

    int ret = system(cmd);
    if (ret != 0)
    {
        fprintf(stderr, LOG_PREFIX "gs conversion failed (exit=%d)\n", ret);
        return -1;
    }

    return 0;
}

/**
 * Convert PDF to XPS
 */
static int pdf_to_xps(const char *pdf_path, const char *xps_path)
{
    char cmd[1024];
    int ret;

    /* Try gxps-convert first (from libgxps-utils) */
    if (command_exists("gxps-convert"))
    {
        snprintf(cmd, sizeof(cmd),
                 "gxps-convert -f xps -o '%s' '%s' >/dev/null 2>&1",
                 xps_path, pdf_path);
        ret = system(cmd);
        if (ret == 0)
        {
            return 0;
        }
    }

    /* Try mutool (from mupdf-tools) */
    if (command_exists("mutool"))
    {
        snprintf(cmd, sizeof(cmd),
                 "mutool convert -o '%s' '%s' >/dev/null 2>&1",
                 xps_path, pdf_path);
        ret = system(cmd);
        if (ret == 0)
        {
            return 0;
        }
    }

    /* Try xpstopdf reverse (if available) */
    fprintf(stderr, LOG_PREFIX "No PDF->XPS converter succeeded\n");
    return -1;
}

/**
 * Convert PostScript data to XPS format
 */
int printer_xps_convert_ps(const uint8_t *ps_data, uint32_t ps_len,
                           uint8_t **xps_data, uint32_t *xps_len,
                           const char *spool_dir)
{
    char ps_path[256];
    char pdf_path[256];
    char xps_path[256];
    int ret = -1;

    if (ps_data == NULL || ps_len == 0 || xps_data == NULL || xps_len == NULL)
    {
        return -1;
    }

    *xps_data = NULL;
    *xps_len = 0;

    /* Create temporary file paths */
    snprintf(ps_path, sizeof(ps_path), "%s/convert_%d.ps",
             spool_dir, getpid());
    snprintf(pdf_path, sizeof(pdf_path), "%s/convert_%d.pdf",
             spool_dir, getpid());
    snprintf(xps_path, sizeof(xps_path), "%s/convert_%d.xps",
             spool_dir, getpid());

    /* Write PS data to file */
    if (write_buffer_to_file(ps_path, ps_data, ps_len) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Failed to write PS temp file\n");
        goto cleanup;
    }

    /* PS -> PDF */
    if (ps_to_pdf(ps_path, pdf_path) != 0)
    {
        fprintf(stderr, LOG_PREFIX "PS->PDF conversion failed\n");
        goto cleanup;
    }

    /* PDF -> XPS */
    if (pdf_to_xps(pdf_path, xps_path) != 0)
    {
        fprintf(stderr, LOG_PREFIX "PDF->XPS conversion failed\n");
        goto cleanup;
    }

    /* Read XPS output */
    if (read_file_to_buffer(xps_path, xps_data, xps_len) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Failed to read XPS output\n");
        goto cleanup;
    }

    fprintf(stderr, LOG_PREFIX "Converted PS (%u bytes) -> XPS (%u bytes)\n",
            ps_len, *xps_len);
    ret = 0;

cleanup:
    unlink(ps_path);
    unlink(pdf_path);
    unlink(xps_path);
    return ret;
}

/**
 * Convert PDF data to XPS format
 */
int printer_xps_convert_pdf(const uint8_t *pdf_data, uint32_t pdf_len,
                            uint8_t **xps_data, uint32_t *xps_len,
                            const char *spool_dir)
{
    char pdf_path[256];
    char xps_path[256];
    int ret = -1;

    if (pdf_data == NULL || pdf_len == 0 || xps_data == NULL || xps_len == NULL)
    {
        return -1;
    }

    *xps_data = NULL;
    *xps_len = 0;

    snprintf(pdf_path, sizeof(pdf_path), "%s/convert_%d.pdf",
             spool_dir, getpid());
    snprintf(xps_path, sizeof(xps_path), "%s/convert_%d.xps",
             spool_dir, getpid());

    /* Write PDF data to file */
    if (write_buffer_to_file(pdf_path, pdf_data, pdf_len) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Failed to write PDF temp file\n");
        goto cleanup;
    }

    /* PDF -> XPS */
    if (pdf_to_xps(pdf_path, xps_path) != 0)
    {
        fprintf(stderr, LOG_PREFIX "PDF->XPS conversion failed\n");
        goto cleanup;
    }

    /* Read XPS output */
    if (read_file_to_buffer(xps_path, xps_data, xps_len) != 0)
    {
        fprintf(stderr, LOG_PREFIX "Failed to read XPS output\n");
        goto cleanup;
    }

    fprintf(stderr, LOG_PREFIX "Converted PDF (%u bytes) -> XPS (%u bytes)\n",
            pdf_len, *xps_len);
    ret = 0;

cleanup:
    unlink(pdf_path);
    unlink(xps_path);
    return ret;
}

/**
 * Detect the format of print data by examining magic bytes
 */
const char *printer_detect_format(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len < 4)
    {
        return "raw";
    }

    /* PostScript: starts with %!PS or %! */
    if (len >= 4 && data[0] == '%' && data[1] == '!')
    {
        return "ps";
    }

    /* PDF: starts with %PDF */
    if (len >= 4 && data[0] == '%' && data[1] == 'P' &&
        data[2] == 'D' && data[3] == 'F')
    {
        return "pdf";
    }

    /* XPS/OOXML: starts with PK (ZIP archive) */
    if (len >= 4 && data[0] == 'P' && data[1] == 'K' &&
        data[2] == 0x03 && data[3] == 0x04)
    {
        return "xps";
    }

    return "raw";
}
