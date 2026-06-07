/**
 * Unit tests for printer_detect_format()
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "printer/printer_xps.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_STR_EQ(expected, actual, msg) do { \
    tests_run++; \
    if (strcmp((expected), (actual)) == 0) { \
        tests_passed++; \
    } else { \
        fprintf(stderr, "FAIL: %s\n  expected: '%s'\n  actual:   '%s'\n", \
                (msg), (expected), (actual)); \
    } \
} while (0)

static void test_null_data(void)
{
    const char *r = printer_detect_format(NULL, 0);
    ASSERT_STR_EQ("raw", r, "null data returns raw");
}

static void test_short_data(void)
{
    uint8_t data[] = {0x25, 0x21, 0x50};
    const char *r = printer_detect_format(data, 3);
    ASSERT_STR_EQ("raw", r, "data shorter than 4 bytes returns raw");
}

static void test_postscript_magic(void)
{
    uint8_t data[] = "%!PS-Adobe-3.0\n%%Title: test";
    const char *r = printer_detect_format(data, (uint32_t)strlen((char *)data));
    ASSERT_STR_EQ("ps", r, "PostScript detected by %! magic");
}

static void test_postscript_minimal(void)
{
    uint8_t data[] = {'%', '!', 'x', 'y'};
    const char *r = printer_detect_format(data, 4);
    ASSERT_STR_EQ("ps", r, "Minimal %! detected as PostScript");
}

static void test_pdf_magic(void)
{
    uint8_t data[] = "%PDF-1.4\n";
    const char *r = printer_detect_format(data, (uint32_t)strlen((char *)data));
    ASSERT_STR_EQ("pdf", r, "PDF detected by %PDF magic");
}

static void test_xps_zip_magic(void)
{
    uint8_t data[] = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00};
    const char *r = printer_detect_format(data, 6);
    ASSERT_STR_EQ("xps", r, "XPS/ZIP detected by PK\\x03\\x04 magic");
}

static void test_raw_unknown(void)
{
    uint8_t data[] = {0x1B, 0x45, 0x00, 0x00, 0x00};
    const char *r = printer_detect_format(data, 5);
    ASSERT_STR_EQ("raw", r, "Unknown magic returns raw");
}

static void test_raw_pcl(void)
{
    /* PCL data (ESC sequence) -- not recognized, should be raw */
    uint8_t data[] = {0x1B, 0x25, 0x2D, 0x31, 0x32, 0x33};
    const char *r = printer_detect_format(data, 6);
    ASSERT_STR_EQ("raw", r, "PCL data returns raw");
}

static void test_pdf_not_ps(void)
{
    /* %PDF starts with %P -- should be PDF not PS, even though %P looks
     * like it could match %! check (it doesn't: second byte is 'P' not '!') */
    uint8_t data[] = "%PDF-2.0 test";
    const char *r = printer_detect_format(data, (uint32_t)strlen((char *)data));
    ASSERT_STR_EQ("pdf", r, "%PDF classified as PDF, not PS");
}

int main(void)
{
    test_null_data();
    test_short_data();
    test_postscript_magic();
    test_postscript_minimal();
    test_pdf_magic();
    test_xps_zip_magic();
    test_raw_unknown();
    test_raw_pcl();
    test_pdf_not_ps();

    printf("test_detect_format: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
