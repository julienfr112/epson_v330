# Makefile for the EPSON Perfection V33/V330 native driver.
#
#   make            build the CLI (v330scan) and the SANE backend
#   sudo make install     install backend, firmware, udev rule and register with SANE
#   sudo make uninstall   remove everything installed
#
# Requires: gcc, libusb-1.0 dev headers, SANE dev headers (libsane-dev).

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDLIBS   = -lusb-1.0 -lm

# multiarch triplet, e.g. x86_64-linux-gnu
TRIPLET := $(shell $(CC) -dumpmachine)

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
FWDIR      ?= /usr/share/v330
SANEDIR    ?= /usr/lib/$(TRIPLET)/sane
SANECONFD  ?= /etc/sane.d
UDEVDIR    ?= /etc/udev/rules.d

BACKEND  = libsane-v330.so.1
CLI      = v330scan
FIRMWARE = esfwad.bin

all: $(CLI) $(BACKEND)

$(CLI): v330scan.c v330_core.c v330_image.c v330_core.h v330_image.h tables.h
	$(CC) $(CFLAGS) -o $@ v330scan.c v330_core.c v330_image.c $(LDLIBS)

$(BACKEND): v330_backend.c v330_core.c v330_image.c v330_core.h v330_image.h tables.h
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(BACKEND) -o $@ \
	    v330_backend.c v330_core.c v330_image.c $(LDLIBS)

install: all
	@echo "Installing to SANE backend dir $(SANEDIR)"
	install -d $(DESTDIR)$(SANEDIR) $(DESTDIR)$(BINDIR) $(DESTDIR)$(FWDIR) \
	           $(DESTDIR)$(SANECONFD)/dll.d $(DESTDIR)$(UDEVDIR)
	install -m 0755 $(BACKEND) $(DESTDIR)$(SANEDIR)/$(BACKEND)
	ln -sf $(BACKEND) $(DESTDIR)$(SANEDIR)/libsane-v330.so
	install -m 0755 $(CLI) $(DESTDIR)$(BINDIR)/$(CLI)
	install -m 0644 $(FIRMWARE) $(DESTDIR)$(FWDIR)/$(FIRMWARE)
	echo v330 > $(DESTDIR)$(SANECONFD)/dll.d/v330
	install -m 0644 99-epson-v330.rules $(DESTDIR)$(UDEVDIR)/99-epson-v330.rules
	-udevadm control --reload-rules 2>/dev/null && udevadm trigger 2>/dev/null
	@echo
	@echo "Installed. Replug the scanner, then test with:  scanimage -L"
	@echo "It will now appear in Simple Scan / GNOME Document Scanner."

uninstall:
	rm -f $(DESTDIR)$(SANEDIR)/$(BACKEND) $(DESTDIR)$(SANEDIR)/libsane-v330.so
	rm -f $(DESTDIR)$(BINDIR)/$(CLI)
	rm -f $(DESTDIR)$(FWDIR)/$(FIRMWARE)
	rm -f $(DESTDIR)$(SANECONFD)/dll.d/v330
	rm -f $(DESTDIR)$(UDEVDIR)/99-epson-v330.rules
	-rmdir $(DESTDIR)$(FWDIR) 2>/dev/null || true
	-udevadm control --reload-rules 2>/dev/null && udevadm trigger 2>/dev/null
	@echo "Uninstalled."

clean:
	rm -f $(CLI) $(BACKEND)

.PHONY: all install uninstall clean
