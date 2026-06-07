#!/bin/sh
#
# Set CUPS_SERVER for xrdp sessions to use the per-session CUPS instance.
# Installed to /etc/profile.d/ -- sourced by all login shells.
#
# Reads /etc/xrdp/sesman.ini to determine if per-session cupsd mode is
# active. If so, unconditionally sets CUPS_SERVER to the expected socket
# path. CUPS clients tolerate a brief "connection refused" while the
# private cupsd is still starting.
#
# When EnablePerSessionCupsd=false in sesman.ini, CUPS_SERVER stays
# unset and apps use the system cupsd.
#

# Only relevant inside xrdp sessions (DISPLAY is :N or :N.M)
if [ -n "$DISPLAY" ]; then
    # Check sesman.ini -- default is per-session cupsd enabled
    SESMAN_INI="/etc/xrdp/sesman.ini"
    _per_session=true
    if [ -f "$SESMAN_INI" ]; then
        _val=$(grep -i '^\s*EnablePerSessionCupsd\s*=' "$SESMAN_INI" 2>/dev/null \
               | tail -1 | sed 's/^[^=]*=\s*//' | tr '[:upper:]' '[:lower:]')
        case "$_val" in
            false|no|0) _per_session=false ;;
        esac
    fi

    if [ "$_per_session" = "true" ]; then
        DISPLAY_NUM=$(echo "$DISPLAY" | sed 's/^://' | sed 's/\..*//')
        UID_NUM=$(id -u)
        CUPS_DIR="/tmp/xrdp-cups-${UID_NUM}-${DISPLAY_NUM}"
        export CUPS_SERVER="${CUPS_DIR}/sock"
    fi

    unset _per_session _val SESMAN_INI
fi
