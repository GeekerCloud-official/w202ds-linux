# SPDX-License-Identifier: MIT
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
PYTHON ?= python3
OUT ?= output
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static

.PHONY: all install
all: $(OUT)/w202-modem-at $(OUT)/w202-modem-netcheck $(OUT)/w202-android-runtime $(OUT)/runtime-properties.bin

$(OUT):
	mkdir -p "$@"

$(OUT)/w202-modem-at: at/w202-modem-at.c | $(OUT)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o "$@"

$(OUT)/w202-modem-netcheck: at/w202-modem-netcheck.c | $(OUT)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o "$@"

$(OUT)/w202-android-runtime: runtime/w202-android-runtime.c | $(OUT)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o "$@"

$(OUT)/runtime-properties.bin: runtime/make-property-area.py runtime/runtime-properties.json | $(OUT)
	$(PYTHON) $< runtime/runtime-properties.json "$@"

# Stage only. Enabling services and provisioning private inputs belong to integration.
install: all
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	mkdir -p "$(DESTDIR)"
	cp -a rootfs/. "$(DESTDIR)/"
	install -m 0755 rootfs/usr/libexec/w202-modem-runtime "$(DESTDIR)/usr/libexec/w202-modem-runtime"
	install -m 0755 runtime/w202-modem-storage.py "$(DESTDIR)/usr/libexec/w202-modem-storage"
	install -m 0755 rootfs/usr/libexec/w202-cellular "$(DESTDIR)/usr/libexec/w202-cellular"
	install -m 0755 rootfs/usr/local/sbin/w202-cellular "$(DESTDIR)/usr/local/sbin/w202-cellular"
	install -D -m 0755 "$(OUT)/w202-modem-at" "$(DESTDIR)/usr/local/sbin/w202-modem-at"
	install -D -m 0755 "$(OUT)/w202-modem-netcheck" "$(DESTDIR)/usr/local/sbin/w202-modem-netcheck"
	install -D -m 0755 "$(OUT)/w202-android-runtime" "$(DESTDIR)/usr/libexec/w202-android-runtime"
	install -D -m 0644 "$(OUT)/runtime-properties.bin" "$(DESTDIR)/usr/share/w202ds/modem/runtime-properties.bin"
