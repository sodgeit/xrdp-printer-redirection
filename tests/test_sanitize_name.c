/**
 * Unit tests for printer_sanitize_name()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "printer/printer.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_STR_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) == NULL && (actual) == NULL) { \
        tests_passed++; \
    } else if ((expected) != NULL && (actual) != NULL && \
               strcmp((expected), (actual)) == 0) { \
        tests_passed++; \
    } else { \
        fprintf(stderr, "FAIL: %s\n  expected: '%s'\n  actual:   '%s'\n", \
                (msg), (expected) ? (expected) : "(null)", \
                (actual) ? (actual) : "(null)"); \
    } \
} while (0)

static void test_basic_name_session_zero(void)
{
    char *r = printer_sanitize_name("HP LaserJet", 0);
    ASSERT_STR_EQ("xrdp_HP_LaserJet", r, "basic name with session_id=0");
    free(r);
}

static void test_basic_name_with_session(void)
{
    char *r = printer_sanitize_name("HP LaserJet", 10);
    ASSERT_STR_EQ("xrdp_10_HP_LaserJet", r, "basic name with session_id=10");
    free(r);
}

static void test_preserves_hyphens(void)
{
    char *r = printer_sanitize_name("Office-Printer", 0);
    ASSERT_STR_EQ("xrdp_Office-Printer", r, "hyphens preserved");
    free(r);
}

static void test_replaces_special_chars(void)
{
    char *r = printer_sanitize_name("My Printer (USB/LPT1)", 0);
    ASSERT_STR_EQ("xrdp_My_Printer__USB_LPT1_", r, "special chars replaced");
    free(r);
}

static void test_null_name(void)
{
    char *r = printer_sanitize_name(NULL, 0);
    ASSERT_STR_EQ("xrdp_printer", r, "null name defaults to 'printer'");
    free(r);
}

static void test_empty_name(void)
{
    char *r = printer_sanitize_name("", 0);
    ASSERT_STR_EQ("xrdp_printer", r, "empty name defaults to 'printer'");
    free(r);
}

static void test_null_name_with_session(void)
{
    char *r = printer_sanitize_name(NULL, 5);
    ASSERT_STR_EQ("xrdp_5_printer", r, "null name with session_id=5");
    free(r);
}

static void test_dots_and_slashes(void)
{
    char *r = printer_sanitize_name("\\\\server\\printer.v2", 0);
    ASSERT_STR_EQ("xrdp___server_printer_v2", r, "backslashes and dots");
    free(r);
}

static void test_alphanumeric_only(void)
{
    char *r = printer_sanitize_name("Printer123", 0);
    ASSERT_STR_EQ("xrdp_Printer123", r, "alphanumeric passthrough");
    free(r);
}

int main(void)
{
    test_basic_name_session_zero();
    test_basic_name_with_session();
    test_preserves_hyphens();
    test_replaces_special_chars();
    test_null_name();
    test_empty_name();
    test_null_name_with_session();
    test_dots_and_slashes();
    test_alphanumeric_only();

    printf("test_sanitize_name: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
