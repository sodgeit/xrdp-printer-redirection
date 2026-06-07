#!/bin/bash
#
# Integration test: verify the built .deb package contents
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEB_PATH="${PROJECT_DIR}/build_deb/xrdp_0.9.24-4+printer1_amd64.deb"

tests_run=0
tests_passed=0

assert_contains() {
    local haystack="$1" needle="$2" msg="$3"
    tests_run=$((tests_run + 1))
    if echo "$haystack" | grep -q "$needle"; then
        tests_passed=$((tests_passed + 1))
    else
        echo "FAIL: $msg" >&2
        echo "  expected to find: '$needle'" >&2
    fi
}

assert_file_in_deb() {
    local path="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if dpkg-deb -c "$DEB_PATH" 2>/dev/null | grep -q "$path"; then
        tests_passed=$((tests_passed + 1))
    else
        echo "FAIL: $msg" >&2
        echo "  file not found in deb: '$path'" >&2
    fi
}

# Find the .deb (handle version bumps)
if [ ! -f "$DEB_PATH" ]; then
    DEB_PATH=$(ls "${PROJECT_DIR}/build_deb"/xrdp_0.9.24-*+printer*_amd64.deb 2>/dev/null | head -1)
fi

if [ -z "$DEB_PATH" ] || [ ! -f "$DEB_PATH" ]; then
    echo "ERROR: No .deb package found. Run scripts/build-deb.sh first." >&2
    exit 1
fi

echo "Testing package: $(basename "$DEB_PATH")"

# --- Test: Required files present ---
assert_file_in_deb "./usr/lib/cups/backend/xrdp-printer" "CUPS backend installed"
assert_file_in_deb "./usr/share/ppd/xrdp/xrdp-printer.ppd" "PPD file installed"
assert_file_in_deb "./usr/share/xrdp/xrdp-printer-cleanup" "Cleanup script installed"
assert_file_in_deb "./usr/share/xrdp/xrdp-cupsd.conf.in" "cupsd config template installed"
assert_file_in_deb "./usr/share/xrdp/xrdp-cups-files.conf.in" "cups-files template installed"
assert_file_in_deb "./usr/share/xrdp/xrdp-cups-forward-printers" "Forward printers script installed"
assert_file_in_deb "./usr/lib/systemd/system/xrdp.service.d/printer-cleanup.conf" "systemd drop-in installed"
assert_file_in_deb "./etc/profile.d/xrdp-cups-env.sh" "profile.d env script installed"
assert_file_in_deb "./etc/xrdp/sesman.ini" "sesman.ini present"

# --- Test: Dependencies ---
DEPS=$(dpkg-deb -f "$DEB_PATH" Depends 2>/dev/null)
assert_contains "$DEPS" "cups" "Depends on cups"
assert_contains "$DEPS" "cups-client" "Depends on cups-client"
assert_contains "$DEPS" "ghostscript" "Depends on ghostscript"
assert_contains "$DEPS" "libgxps-utils\|mupdf-tools" "Depends on libgxps-utils|mupdf-tools"

# --- Test: sesman.ini contains our config ---
SESMAN_INI=$(dpkg-deb --fsys-tarfile "$DEB_PATH" 2>/dev/null | tar -xO ./etc/xrdp/sesman.ini 2>/dev/null)
assert_contains "$SESMAN_INI" "EnablePrinterRedirection" "sesman.ini has EnablePrinterRedirection"
assert_contains "$SESMAN_INI" "EnablePerSessionCupsd" "sesman.ini has EnablePerSessionCupsd"

# --- Test: Man page contains our docs ---
MAN_PAGE=$(dpkg-deb --fsys-tarfile "$DEB_PATH" 2>/dev/null | tar -xO ./usr/share/man/man5/sesman.ini.5.gz 2>/dev/null | gunzip)
assert_contains "$MAN_PAGE" "EnablePrinterRedirection" "Man page documents EnablePrinterRedirection"
assert_contains "$MAN_PAGE" "EnablePerSessionCupsd" "Man page documents EnablePerSessionCupsd"

# --- Test: CUPS backend has correct permissions (0700 or 0755 after dh_fixperms) ---
tests_run=$((tests_run + 1))
BACKEND_PERMS=$(dpkg-deb -c "$DEB_PATH" 2>/dev/null | grep "xrdp-printer$" | awk '{print $1}')
if [ "$BACKEND_PERMS" = "-rwx------" ] || [ "$BACKEND_PERMS" = "-rwxr-xr-x" ]; then
    tests_passed=$((tests_passed + 1))
else
    echo "FAIL: CUPS backend permissions should be 0700 or 0755" >&2
    echo "  actual: '$BACKEND_PERMS'" >&2
fi

# --- Test: profile.d script reads sesman.ini ---
ENV_SCRIPT=$(dpkg-deb --fsys-tarfile "$DEB_PATH" 2>/dev/null | tar -xO ./etc/profile.d/xrdp-cups-env.sh 2>/dev/null)
assert_contains "$ENV_SCRIPT" "sesman.ini" "env script reads sesman.ini"
assert_contains "$ENV_SCRIPT" "EnablePerSessionCupsd" "env script checks EnablePerSessionCupsd"

echo "test_deb_package: ${tests_passed}/${tests_run} passed"
[ "$tests_passed" -eq "$tests_run" ]
