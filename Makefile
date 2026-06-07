# Makefile for xrdp printer redirection module
#
# Targets:
#   all       - Build all components
#   backend   - Build the CUPS backend only
#   lib       - Build the printer library only
#   install   - Install all components
#   uninstall - Remove installed components
#   clean     - Remove build artifacts

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -Wl,-z,relro -Wl,-z,now

# Installation paths
PREFIX ?= /usr
CUPS_BACKEND_DIR ?= /usr/lib/cups/backend
CUPS_PPD_DIR ?= /usr/share/ppd/xrdp
XRDP_SHARE_DIR ?= /usr/share/xrdp
XRDP_LIB_DIR ?= /usr/lib/x86_64-linux-gnu/xrdp

# Source files
PRINTER_SRCS = src/printer/printer.c src/printer/printer_cups.c \
               src/printer/printer_cups_session.c \
               src/printer/printer_socket.c src/printer/printer_xps.c
DEVREDIR_SRCS = src/devredir_printer.c
BACKEND_SRCS = src/cups/xrdp-printer.c

# Object files
PRINTER_OBJS = $(PRINTER_SRCS:.c=.o)
DEVREDIR_OBJS = $(DEVREDIR_SRCS:.c=.o)
BACKEND_OBJS = $(BACKEND_SRCS:.c=.o)

# Include paths
INCLUDES = -Isrc

# Standalone build flag (for compilation without xrdp source tree)
# In standalone mode, some functions are unused since they're only called
# from the IRP callback which is compiled out
STANDALONE_FLAGS = -DSTANDALONE_BUILD -Wno-unused-function

# Targets
BACKEND_BIN = xrdp-printer
LIB_NAME = libxrdp_printer.a

.PHONY: all backend lib clean install uninstall test

all: backend lib

backend: $(BACKEND_BIN)

lib: $(LIB_NAME)

# Test binaries
TEST_SANITIZE = tests/test_sanitize_name
TEST_FORMAT = tests/test_detect_format
TEST_ANNOUNCE = tests/test_device_announce

test: $(TEST_SANITIZE) $(TEST_FORMAT) $(TEST_ANNOUNCE)
	@echo "=== Running C unit tests ==="
	./$(TEST_SANITIZE)
	./$(TEST_FORMAT)
	./$(TEST_ANNOUNCE)
	@echo "=== Running shell tests ==="
	bash tests/test_cups_env.sh
	@echo "=== All tests passed ==="

$(TEST_SANITIZE): tests/test_sanitize_name.c src/printer/printer.c
	$(CC) $(CFLAGS) $(INCLUDES) $(STANDALONE_FLAGS) -o $@ $^

$(TEST_FORMAT): tests/test_detect_format.c src/printer/printer_xps.c
	$(CC) $(CFLAGS) $(INCLUDES) $(STANDALONE_FLAGS) -o $@ $^

$(TEST_ANNOUNCE): tests/test_device_announce.c
	$(CC) $(CFLAGS) -o $@ $<

$(BACKEND_BIN): $(BACKEND_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(LIB_NAME): $(PRINTER_OBJS) $(DEVREDIR_OBJS)
	ar rcs $@ $^

# Compile rules
src/cups/%.o: src/cups/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

src/printer/%.o: src/printer/%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(STANDALONE_FLAGS) -c -o $@ $<

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(STANDALONE_FLAGS) -c -o $@ $<

# Installation
install: all
	@echo "Installing xrdp printer redirection..."
	install -d $(DESTDIR)$(CUPS_BACKEND_DIR)
	install -m 0700 $(BACKEND_BIN) $(DESTDIR)$(CUPS_BACKEND_DIR)/xrdp-printer
	install -d $(DESTDIR)$(XRDP_SHARE_DIR)
	install -m 0644 src/cups/xrdp-printer.ppd $(DESTDIR)$(XRDP_SHARE_DIR)/xrdp-printer.ppd
	install -d $(DESTDIR)$(CUPS_PPD_DIR)
	install -m 0644 src/cups/xrdp-printer.ppd $(DESTDIR)$(CUPS_PPD_DIR)/xrdp-printer.ppd
	@echo "Installation complete."
	@echo ""
	@echo "Next steps:"
	@echo "  1. Restart CUPS: sudo systemctl restart cups"
	@echo "  2. Ensure xrdp chansrv is patched with printer support"

uninstall:
	@echo "Removing xrdp printer redirection..."
	rm -f $(DESTDIR)$(CUPS_BACKEND_DIR)/xrdp-printer
	rm -f $(DESTDIR)$(XRDP_SHARE_DIR)/xrdp-printer.ppd
	rm -f $(DESTDIR)$(CUPS_PPD_DIR)/xrdp-printer.ppd
	rmdir $(DESTDIR)$(CUPS_PPD_DIR) 2>/dev/null || true
	@echo "Uninstall complete."

clean:
	rm -f $(PRINTER_OBJS) $(DEVREDIR_OBJS) $(BACKEND_OBJS)
	rm -f $(BACKEND_BIN) $(LIB_NAME)
	rm -f $(TEST_SANITIZE) $(TEST_FORMAT) $(TEST_ANNOUNCE)
