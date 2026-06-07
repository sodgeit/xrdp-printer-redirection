/**
 * Unit tests for MS-RDPEFS device data parsing in devredir_printer_device_announce()
 *
 * Since the parsing logic is embedded in a function that requires
 * g_initialized, we test it by calling through the public API after
 * a minimal init, or by replicating the parse logic here for isolation.
 * We use the replication approach to test the parser independently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) == (actual)) { \
        tests_passed++; \
    } else { \
        fprintf(stderr, "FAIL: %s\n  expected: %d\n  actual:   %d\n", \
                (msg), (int)(expected), (int)(actual)); \
    } \
} while (0)

#define ASSERT_STR_EQ(expected, actual, msg) do { \
    tests_run++; \
    if (strcmp((expected), (actual)) == 0) { \
        tests_passed++; \
    } else { \
        fprintf(stderr, "FAIL: %s\n  expected: '%s'\n  actual:   '%s'\n", \
                (msg), (expected), (actual)); \
    } \
} while (0)

/* Replicate the parse logic from devredir_printer_device_announce()
 * to test it in isolation. Returns 0 on success, -1 on failure.
 * On success, fills printer_name and driver_name buffers. */
static int parse_device_data(const uint8_t *device_data, uint32_t device_data_len,
                             char *printer_name, size_t pname_size,
                             char *driver_name, size_t dname_size,
                             uint32_t *flags_out)
{
    if (device_data == NULL || device_data_len < 24)
    {
        return -1;
    }

    const uint8_t *p = device_data;

    uint32_t flags = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    /* code_page - skip */
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
        return -1;
    }

    /* Skip PnP name */
    p += pnp_name_len;

    /* Extract driver name (Unicode LE to ASCII) */
    driver_name[0] = '\0';
    if (driver_name_len >= 2)
    {
        uint32_t j = 0;
        for (uint32_t i = 0; i + 1 < driver_name_len && j < dname_size - 1; i += 2)
        {
            uint8_t lo = p[i];
            uint8_t hi = p[i + 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7f)
            {
                driver_name[j++] = (char)lo;
            }
            else if (hi == 0 && lo == 0)
            {
                break;
            }
        }
        driver_name[j] = '\0';
    }
    p += driver_name_len;

    /* Extract printer name (Unicode LE to ASCII) */
    printer_name[0] = '\0';
    if (printer_name_len >= 2)
    {
        uint32_t j = 0;
        for (uint32_t i = 0; i + 1 < printer_name_len && j < pname_size - 1; i += 2)
        {
            uint8_t lo = p[i];
            uint8_t hi = p[i + 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7f)
            {
                printer_name[j++] = (char)lo;
            }
            else if (hi == 0 && lo == 0)
            {
                break;
            }
        }
        printer_name[j] = '\0';
    }

    if (flags_out)
    {
        *flags_out = flags;
    }
    return 0;
}

/* Helper: write a uint32_t in little-endian into buffer */
static void put_le32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* Helper: write a UTF-16LE string (null terminated) into buffer.
 * Returns total bytes written including the null terminator (2 bytes). */
static uint32_t put_utf16le(uint8_t *buf, const char *str)
{
    uint32_t i = 0;
    while (*str)
    {
        buf[i] = (uint8_t)*str;
        buf[i + 1] = 0;
        i += 2;
        str++;
    }
    buf[i] = 0;
    buf[i + 1] = 0;
    return i + 2;
}

/* Build a minimal MS-RDPEFS printer device_data packet */
static uint32_t build_device_data(uint8_t *buf, size_t bufsize,
                                  uint32_t flags,
                                  const char *pnp_name,
                                  const char *driver_name,
                                  const char *printer_name)
{
    /* Temporary buffers for UTF-16LE strings */
    uint8_t pnp_buf[512], drv_buf[512], prn_buf[512];
    uint32_t pnp_len = put_utf16le(pnp_buf, pnp_name);
    uint32_t drv_len = put_utf16le(drv_buf, driver_name);
    uint32_t prn_len = put_utf16le(prn_buf, printer_name);
    uint32_t cached_len = 0;

    uint32_t total = 24 + pnp_len + drv_len + prn_len + cached_len;
    if (total > bufsize)
    {
        return 0;
    }

    uint8_t *p = buf;
    put_le32(p, flags); p += 4;      /* Flags */
    put_le32(p, 0); p += 4;          /* CodePage */
    put_le32(p, pnp_len); p += 4;    /* PnPNameLen */
    put_le32(p, drv_len); p += 4;    /* DriverNameLen */
    put_le32(p, prn_len); p += 4;    /* PrinterNameLen */
    put_le32(p, cached_len); p += 4; /* CachedFieldsLen */

    memcpy(p, pnp_buf, pnp_len); p += pnp_len;
    memcpy(p, drv_buf, drv_len); p += drv_len;
    memcpy(p, prn_buf, prn_len); p += prn_len;

    return total;
}

/* --- Tests --- */

static void test_null_data(void)
{
    char pn[256], dn[256];
    int ret = parse_device_data(NULL, 0, pn, sizeof(pn), dn, sizeof(dn), NULL);
    ASSERT_EQ(-1, ret, "null device_data returns -1");
}

static void test_too_short(void)
{
    uint8_t data[20] = {0};
    char pn[256], dn[256];
    int ret = parse_device_data(data, 20, pn, sizeof(pn), dn, sizeof(dn), NULL);
    ASSERT_EQ(-1, ret, "data < 24 bytes returns -1");
}

static void test_length_overflow(void)
{
    /* Header claims fields that exceed total buffer length */
    uint8_t data[24] = {0};
    put_le32(data + 0, 0x01);   /* flags */
    put_le32(data + 4, 0);      /* code_page */
    put_le32(data + 8, 100);    /* pnp_name_len (bogus: exceeds buffer) */
    put_le32(data + 12, 0);     /* driver_name_len */
    put_le32(data + 16, 0);     /* printer_name_len */
    put_le32(data + 20, 0);     /* cached_fields_len */

    char pn[256], dn[256];
    int ret = parse_device_data(data, 24, pn, sizeof(pn), dn, sizeof(dn), NULL);
    ASSERT_EQ(-1, ret, "length overflow returns -1");
}

static void test_valid_basic(void)
{
    uint8_t buf[1024];
    uint32_t len = build_device_data(buf, sizeof(buf), 0x01,
                                     "PnP", "HP Driver", "HP LaserJet");

    char pn[256], dn[256];
    uint32_t flags = 0;
    int ret = parse_device_data(buf, len, pn, sizeof(pn), dn, sizeof(dn), &flags);
    ASSERT_EQ(0, ret, "valid packet parses successfully");
    ASSERT_STR_EQ("HP LaserJet", pn, "printer name extracted");
    ASSERT_STR_EQ("HP Driver", dn, "driver name extracted");
    ASSERT_EQ(0x01, (int)flags, "flags extracted correctly");
}

static void test_flags_xps(void)
{
    uint8_t buf[1024];
    uint32_t len = build_device_data(buf, sizeof(buf), 0x10,
                                     "", "XPS Driver", "My XPS Printer");

    char pn[256], dn[256];
    uint32_t flags = 0;
    int ret = parse_device_data(buf, len, pn, sizeof(pn), dn, sizeof(dn), &flags);
    ASSERT_EQ(0, ret, "XPS printer parses successfully");
    ASSERT_STR_EQ("My XPS Printer", pn, "XPS printer name");
    ASSERT_EQ(0x10, (int)flags, "XPS flag set");
}

static void test_empty_names(void)
{
    uint8_t buf[1024];
    uint32_t len = build_device_data(buf, sizeof(buf), 0x02, "", "", "");

    char pn[256], dn[256];
    int ret = parse_device_data(buf, len, pn, sizeof(pn), dn, sizeof(dn), NULL);
    ASSERT_EQ(0, ret, "empty names parse OK");
    /* Empty UTF-16LE still has null terminator -- results in empty string */
    ASSERT_EQ(0, (int)strlen(pn), "printer name is empty");
    ASSERT_EQ(0, (int)strlen(dn), "driver name is empty");
}

static void test_non_ascii_skipped(void)
{
    /* Build a packet with a printer name containing a non-ASCII char (ü = 0xFC) */
    uint8_t manual[64];
    uint8_t *p = manual;
    put_le32(p, 0x01); p += 4;  /* flags */
    put_le32(p, 0); p += 4;     /* code_page */
    put_le32(p, 2); p += 4;     /* pnp_name_len (just null term) */
    put_le32(p, 2); p += 4;     /* driver_name_len (just null term) */
    put_le32(p, 8); p += 4;     /* printer_name_len: "Tü" = T,0 | FC,00 | 0,0 | pad */
    put_le32(p, 0); p += 4;     /* cached_fields_len */
    /* PnP: null-terminated UTF-16 */
    *p++ = 0; *p++ = 0;
    /* Driver: null-terminated UTF-16 */
    *p++ = 0; *p++ = 0;
    /* Printer: 'T', 0x00, 0xFC, 0x00, 0x00, 0x00, pad, pad */
    *p++ = 'T'; *p++ = 0x00;
    *p++ = 0xFC; *p++ = 0x00;   /* non-ASCII: ü (>= 0x7f, skipped) */
    *p++ = 0x00; *p++ = 0x00;   /* null terminator */
    *p++ = 0x00; *p++ = 0x00;   /* extra padding to reach 8 bytes */

    uint32_t total = (uint32_t)(p - manual);
    char pn[256], dn[256];
    int ret = parse_device_data(manual, total, pn, sizeof(pn), dn, sizeof(dn), NULL);
    ASSERT_EQ(0, ret, "non-ASCII packet parses OK");
    ASSERT_STR_EQ("T", pn, "non-ASCII char skipped, only 'T' remains");
}

int main(void)
{
    test_null_data();
    test_too_short();
    test_length_overflow();
    test_valid_basic();
    test_flags_xps();
    test_empty_names();
    test_non_ascii_skipped();

    printf("test_device_announce: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
