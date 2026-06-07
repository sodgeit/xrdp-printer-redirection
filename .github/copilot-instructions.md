# Copilot Instructions

## Project: XRDP Printer Redirection

This file contains project-wide instructions for GitHub Copilot. It is automatically included in every request.

## Self-Maintenance Rule

**Always update this file** when:
- A new requirement is established
- A design decision is made
- A convention or pattern is adopted
- A dependency or tool is added
- Any information relevant to future development is discovered

Keep this file as the single source of truth for project context.

## Requirements

- Add printer redirection support to xrdp 0.9.24
- Target platform: Ubuntu 24.04
- Support RAW and XPS print job formats
- Work with mstsc and Remmina clients
- Integrate via CUPS virtual printers + custom CUPS backend
- Communication between CUPS backend and chansrv via Unix domain socket

## Architecture & Design Decisions

- **Approach**: CUPS virtual printer per redirected client printer
- **Backend**: Custom CUPS backend at `/usr/lib/cups/backend/xrdp-printer`
- **IPC**: Unix domain socket at `/run/xrdp/sockdir/printer_<session_id>`
- **Protocol**: Uses MS-RDPEFS RDPDR channel, RDPDR_DTYP_PRINT device type
- **IRP flow**: IRP_MJ_CREATE -> IRP_MJ_WRITE (chunked 64KB) -> IRP_MJ_CLOSE
- **Device data parsing**: Per MS-RDPEFS 2.2.3.1 (Flags, CodePage, PnPName, DriverName, PrinterName, CachedData)
- **Integration point**: Extends devredir.c in xrdp's chansrv
- **Queue naming**: Configurable via `EnablePerSessionCupsd` in sesman.ini `[Chansrv]` section:
  - Per-session cupsd (default): `xrdp_<sanitized_name>` -- stable across sessions
  - System cupsd: `xrdp_<display>_<sanitized_name>` -- includes display for isolation
- **Configuration**: `EnablePrinterRedirection` (default true), `EnablePerSessionCupsd` (default true) in `[Chansrv]` section of sesman.ini

## Conventions

- C99, Apache 2.0 license headers
- `LOG_PREFIX` macros for stderr logging
- xrdp coding style (4-space indent, braces on own line)
- Build with `-Wall -Wextra` and zero warnings

## Dependencies & Tools

- Build: gcc, make
- Runtime: cups, cups-client, ghostscript (for XPS)
- Dev: libcups2-dev
- xrdp 0.9.24 (target system package)

## Notes

- Per-session cupsd socket path is always `/tmp/xrdp-cups-<uid>-<display>/sock`
- `/etc/profile.d/xrdp-cups-env.sh` reads sesman.ini directly to determine whether to set `CUPS_SERVER` -- no race condition with chansrv startup
- XPS support requires PDF intermediate: app -> PostScript -> PDF (gs) -> XPS (gxps)
- CUPS backend runs as lp user; socket permissions must allow connection
- On package upgrades, dpkg may prompt about sesman.ini changes; `--force-confnew` replaces cleanly
