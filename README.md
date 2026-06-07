# XRDP Printer Redirection

Printer redirection support for xrdp 0.9.24, enabling RDP clients to print
to their local printers from within an xrdp session.

## Overview

This module adds printer redirection to xrdp by:

1. Launching a **per-session CUPS daemon** so each user's printers are isolated
2. Handling `RDPDR_DTYP_PRINT` device announcements from the RDP client
3. Creating CUPS virtual printer queues for each redirected printer
4. Forwarding system printers into the private CUPS instance
5. Intercepting print jobs via a custom CUPS backend
6. Sending print data back to the client via the RDPDR channel using
   IRP_MJ_CREATE / IRP_MJ_WRITE / IRP_MJ_CLOSE sequences

### Per-Session Isolation

Each xrdp session runs its own `cupsd` instance listening on a private Unix
socket. This provides:

- **Privacy**  -- users only see their own redirected printers plus system printers
- **Stable queue names**  -- queues are named `xrdp_<sanitized_name>` (e.g.
  `xrdp_HP_LaserJet_Pro`). Because there's no display number in the name,
  user preferences (paper size, default tray, etc.) persist across sessions
  even if the display number changes.
- **No conflicts**  -- multiple concurrent sessions cannot interfere with each
  other's printer queues

## Supported Formats

- **RAW** - Direct passthrough of print data
- **XPS** - XML Paper Specification (via PDF intermediate conversion)

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        xrdp Server                              │
│                                                                 │
│  ┌──────────┐    ┌──────────────┐    ┌────────────────────────┐ │
│  │ App      │───>│ CUPS Virtual │───>│ xrdp-printer backend   │ │
│  │ (prints) │    │ Printer      │    │ (captures job data)    │ │
│  └──────────┘    └──────────────┘    └───────────┬────────────┘ │
│                                                  │              │
│                                      ┌───────────▼────────────┐ │
│                                      │ chansrv / devredir     │ │
│                                      │ (sends IRPs via RDPDR) │ │
│                                      └───────────┬────────────┘ │
└──────────────────────────────────────────────────┼──────────────┘
                                                   │ RDPDR Channel
┌──────────────────────────────────────────────────┼──────────────┐
│                        RDP Client                │              │
│                                      ┌───────────▼────────────┐ │
│                                      │ Client Printer Driver  │ │
│                                      │ (prints to local HW)   │ │
│                                      └────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

## Requirements

- Ubuntu 24.04
- xrdp 0.9.24 (Ubuntu package)
- CUPS (Common Unix Printing System)
- Ghostscript (for XPS format support)

## Development Setup

This project uses a VS Code devcontainer that includes all build dependencies
and the xrdp Ubuntu source package (with patches applied).

1. Open in VS Code with the Dev Containers extension
2. The container provides:
   - All xrdp build dependencies (`apt-get build-dep xrdp`)
   - xrdp Ubuntu source at `/usr/src/xrdp/xrdp-0.9.24/`
   - devscripts, quilt, fakeroot for deb packaging

## Building

### Modified xrdp .deb package (recommended)

Builds a drop-in replacement `xrdp` package with printer redirection baked in:

```bash
bash scripts/build-deb.sh
```

Output: `build_deb/xrdp_0.9.24-4+printer1_amd64.deb`

The package includes:
- `xrdp-chansrv` with printer redirection support
- `/usr/lib/cups/backend/xrdp-printer` (CUPS backend)
- `/usr/share/ppd/xrdp/xrdp-printer.ppd` (printer description)

### Standalone build (for development/testing)

Builds the CUPS backend and a static library independently:

```bash
make
```

Produces:
- `xrdp-printer`  -- CUPS backend binary
- `libxrdp_printer.a`  -- static library (standalone mode)

## Installation

Install the modified package on an Ubuntu 24.04 system running xrdp:

```bash
sudo apt install ./build_deb/xrdp_0.9.24-4+printer1_amd64.deb
sudo systemctl restart xrdp
```

`apt install ./` resolves and pulls in all dependencies automatically
(cups, ghostscript, libgxps-utils, etc.). Alternatively, use `dpkg -i`
followed by `sudo apt install -f` to fix missing dependencies after the fact.

## Configuration

Printer redirection is controlled via two settings in `/etc/xrdp/sesman.ini`
under the `[Chansrv]` section:

```ini
[Chansrv]
; Enable or disable printer redirection entirely (default: true)
EnablePrinterRedirection=true

; Use a per-session cupsd for printer isolation (default: true)
; When false, printers are created on the system cupsd instead.
EnablePerSessionCupsd=true
```

### Per-Session CUPS Mode (default)

When `EnablePerSessionCupsd=true`:
- Each session launches its own `cupsd` on a private Unix socket
- Queue names are **stable**: `xrdp_<sanitized_name>` (e.g. `xrdp_HP_LaserJet_Pro`)
- User preferences (paper size, duplex, etc.) persist across sessions
- System printers are automatically forwarded into the private instance
- No `lpadmin` group membership required
- Full session isolation  -- users cannot see each other's printers

### System CUPS Mode

When `EnablePerSessionCupsd=false`:
- Printers are created on the system-wide `cupsd`
- Queue names include the display number: `xrdp_<display>_<sanitized_name>`
  to avoid conflicts between concurrent sessions
- Each queue is ACL-restricted to the session user (`lpadmin -u allow:<user>`),
  so other users cannot print to or see the queue
- The session user **must** be in the `lpadmin` group:
  ```bash
  sudo usermod -aG lpadmin <username>
  ```
- Useful when per-session cupsd is undesirable (e.g., thin clients, minimal
  resource usage, Snap applications)

### Queue Naming Summary

| Mode | Format | Example | Stable Naming |
|------|--------|---------|---------|
| Per-session cupsd | `xrdp_<name>` | `xrdp_HP_LaserJet_Pro` | Yes |
| System cupsd | `xrdp_<display>_<name>` | `xrdp_10_HP_LaserJet_Pro` | No |

### Permissions

The socket directory `/run/xrdp/sockdir` is configured with mode `3777` by
the xrdp package, allowing both the session user and the CUPS backend (`lp`
user) full access.

## File Structure

```
src/
├── devredir_printer.c         # IRP state machine and xrdp integration
├── devredir_printer.h         # Public API for chansrv integration
├── printer/
│   ├── printer.c              # Core printer device management
│   ├── printer.h              # Printer data structures and API
│   ├── printer_cups.c         # CUPS queue creation/removal + ACL
│   ├── printer_cups.h         # CUPS integration header
│   ├── printer_cups_session.c # Per-session cupsd lifecycle
│   ├── printer_cups_session.h # Session cupsd header
│   ├── printer_socket.c       # Unix socket IPC with CUPS backend
│   ├── printer_socket.h       # Socket handler header
│   ├── printer_xps.c          # XPS format detection and conversion
│   └── printer_xps.h          # XPS conversion header
├── cups/
│   ├── xrdp-printer.c         # CUPS backend executable source
│   ├── xrdp-printer.ppd       # Generic PostScript PPD
│   ├── xrdp-cupsd.conf.in     # Per-session cupsd config template
│   ├── xrdp-cups-files.conf.in # Per-session file paths template
│   ├── xrdp-cups-env.sh       # /etc/profile.d script (sets CUPS_SERVER)
│   ├── xrdp-cups-forward-printers # Forwards system printers into session
│   ├── xrdp-printer-cleanup   # Removes stale queues at xrdp start
│   └── xrdp-printer-cleanup.conf # systemd drop-in for xrdp.service
tests/
├── test_sanitize_name.c       # Unit tests for queue name sanitization
├── test_detect_format.c       # Unit tests for XPS/RAW format detection
├── test_device_announce.c     # Unit tests for RDPDR device data parsing
├── test_cups_env.sh           # Shell tests for profile.d CUPS_SERVER logic
└── test_deb_package.sh        # Integration tests for built .deb contents
patches/
├── 01-devredir-printer-support.patch  # devredir.c modifications
├── 02-chansrv-event-loop.patch        # chansrv.c event loop integration
├── 03-makefile-am.patch               # Makefile.am source list
├── 04-chansrv-config-printer.patch    # chansrv_config: printer settings
scripts/
├── build-deb.sh               # Automated deb package builder
Makefile                       # Standalone build + test runner
LICENSE                        # Apache License 2.0
.github/
└── workflows/
    └── build.yml              # CI: build, unit tests, deb package tests
.devcontainer/
├── Dockerfile                 # Build environment with all deps
└── devcontainer.json          # VS Code devcontainer config
```

## How It Works

1. Client connects and announces printers via RDPDR channel (`RDPDR_DTYP_PRINT`)
2. `devredir_printer.c` parses device data (printer name, driver, flags)
3. An async helper child starts a per-session `cupsd`, creates CUPS virtual
   queues (`xrdp_<sanitized_name>`) with the `xrdp-printer://` backend URI,
   and forwards system printers into the private instance
4. The session environment gets `CUPS_SERVER` pointed at the private socket
5. When the user prints, CUPS invokes the `xrdp-printer` backend
6. The backend connects to chansrv via a Unix socket and sends job data
7. chansrv sends the data to the client via IRP_MJ_CREATE -> IRP_MJ_WRITE -> IRP_MJ_CLOSE
8. The client's printer driver renders and prints locally
9. On disconnect, all `xrdp_*` queues are removed and the private cupsd stops

## Testing

Tested with:
- **Server**: Ubuntu 24.04 with xrdp 0.9.24
- **Clients**: mstsc (Windows), Remmina (Linux)
- **Formats**: RAW, XPS

## Patches

The patches modify the xrdp source (applied on top of Ubuntu's patches):

| Patch | File | Purpose |
|-------|------|---------|
| 01 | `devredir.c` | Add printer device announce/remove handling, init/cleanup |
| 02 | `chansrv.c` | Add printer socket FDs to event loop |
| 03 | `Makefile.am` | Add printer source files to build |
| 04 | `chansrv_config.c/h` | Add `EnablePrinterRedirection` and `EnablePerSessionCupsd` settings |

## License

Apache License, Version 2.0
