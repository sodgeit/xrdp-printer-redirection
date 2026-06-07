#!/bin/bash
#
# Build a modified xrdp .deb package with printer redirection support.
# Requires: devscripts, dpkg-dev, fakeroot, debhelper, quilt
# (all installed via the devcontainer Dockerfile)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build_deb"
SRC_DIR="/usr/src/xrdp/xrdp-0.9.24"
PRINTER_VERSION="1"

echo "=== Building xrdp .deb with printer redirection ==="
echo "Project: ${PROJECT_DIR}"
echo "Build:   ${BUILD_DIR}"

# Clean previous build
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# Copy the Ubuntu source tree
echo "=== Copying xrdp source ==="
cp -a "${SRC_DIR}" "${BUILD_DIR}/xrdp-0.9.24"
cd "${BUILD_DIR}/xrdp-0.9.24"

# Unapply existing quilt patches so we can add ours on top
quilt pop -a 2>/dev/null || true

# Copy our printer source files into chansrv
echo "=== Adding printer redirection source files ==="
cp "${PROJECT_DIR}/src/devredir_printer.h" sesman/chansrv/
cp "${PROJECT_DIR}/src/devredir_printer.c" sesman/chansrv/
mkdir -p sesman/chansrv/printer
cp "${PROJECT_DIR}/src/printer/printer.h" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer.c" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_cups.h" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_cups.c" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_cups_session.h" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_cups_session.c" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_socket.h" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_socket.c" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_xps.h" sesman/chansrv/printer/
cp "${PROJECT_DIR}/src/printer/printer_xps.c" sesman/chansrv/printer/

# Copy CUPS backend source, PPD, config templates, and scripts
cp "${PROJECT_DIR}/src/cups/xrdp-printer.c" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-printer.ppd" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-printer-cleanup" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-printer-cleanup.conf" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-cupsd.conf.in" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-cups-files.conf.in" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-cups-env.sh" sesman/chansrv/
cp "${PROJECT_DIR}/src/cups/xrdp-cups-forward-printers" sesman/chansrv/

# Add our patches to debian/patches
echo "=== Adding patches ==="
cp "${PROJECT_DIR}/patches/01-devredir-printer-support.patch" debian/patches/
cp "${PROJECT_DIR}/patches/02-chansrv-event-loop.patch" debian/patches/
cp "${PROJECT_DIR}/patches/03-makefile-am.patch" debian/patches/
cp "${PROJECT_DIR}/patches/04-chansrv-config-printer.patch" debian/patches/

# Append to series
echo "01-devredir-printer-support.patch" >> debian/patches/series
echo "02-chansrv-event-loop.patch" >> debian/patches/series
echo "03-makefile-am.patch" >> debian/patches/series
echo "04-chansrv-config-printer.patch" >> debian/patches/series

# Re-apply all patches (Ubuntu + ours)
echo "=== Applying all patches ==="
quilt push -a

# Add CUPS backend build and install to debian/rules
echo "=== Patching debian/rules for CUPS backend ==="

# Add printer redirection dependencies to debian/control
echo "=== Adding printer redirection dependencies ==="
sed -i '/^ ssl-cert,$/a\ cups,\n cups-client,\n ghostscript,\n libgxps-utils | mupdf-tools,' debian/control

# Add override_dh_auto_build to compile the CUPS backend
# Note: heredoc with <<-'EOF' strips leading tabs; we use sed to add them back
{
  echo ''
  echo 'override_dh_auto_build:'
  printf '\tdh_auto_build\n'
  printf '\t# Build xrdp-printer CUPS backend\n'
  printf '\t$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o sesman/chansrv/xrdp-printer sesman/chansrv/xrdp-printer.c\n'
} >> debian/rules

# Insert our install steps after dh_auto_install in override_dh_auto_install
sed -i '/dh_auto_install.*--destdir=debian\/xrdp/a\\t# --- xrdp printer redirection install ---\n\tmkdir -p debian/xrdp/usr/lib/cups/backend\n\tinstall -m 0700 sesman/chansrv/xrdp-printer debian/xrdp/usr/lib/cups/backend/xrdp-printer\n\tmkdir -p debian/xrdp/usr/share/ppd/xrdp\n\tinstall -m 0644 sesman/chansrv/xrdp-printer.ppd debian/xrdp/usr/share/ppd/xrdp/xrdp-printer.ppd\n\tmkdir -p debian/xrdp/usr/share/xrdp\n\tinstall -m 0755 sesman/chansrv/xrdp-printer-cleanup debian/xrdp/usr/share/xrdp/xrdp-printer-cleanup\n\tinstall -m 0644 sesman/chansrv/xrdp-cupsd.conf.in debian/xrdp/usr/share/xrdp/xrdp-cupsd.conf.in\n\tinstall -m 0644 sesman/chansrv/xrdp-cups-files.conf.in debian/xrdp/usr/share/xrdp/xrdp-cups-files.conf.in\n\tinstall -m 0755 sesman/chansrv/xrdp-cups-forward-printers debian/xrdp/usr/share/xrdp/xrdp-cups-forward-printers\n\tmkdir -p debian/xrdp/usr/lib/systemd/system/xrdp.service.d\n\tinstall -m 0644 sesman/chansrv/xrdp-printer-cleanup.conf debian/xrdp/usr/lib/systemd/system/xrdp.service.d/printer-cleanup.conf\n\tmkdir -p debian/xrdp/etc/profile.d\n\tinstall -m 0644 sesman/chansrv/xrdp-cups-env.sh debian/xrdp/etc/profile.d/xrdp-cups-env.sh' debian/rules

# Update changelog with our modification
echo "=== Updating changelog ==="
DEBFULLNAME="XRDP Printer Redirection" \
DEBEMAIL="noreply@local" \
dch --newversion "$(dpkg-parsechangelog -S Version)+printer${PRINTER_VERSION}" \
    "Add printer redirection support (RAW and XPS)"

# Build the package
echo "=== Building .deb package ==="
dpkg-buildpackage -us -uc -b -j$(nproc)

echo ""
echo "=== Build complete ==="
echo "Packages available in: ${BUILD_DIR}/"
ls -la "${BUILD_DIR}"/*.deb 2>/dev/null || echo "(no .deb found - check build output above)"
