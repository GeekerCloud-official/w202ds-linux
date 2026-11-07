# SPDX-License-Identifier: MIT
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OUT ?= output
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static -s
FIRMWARE := ../firmware

.PHONY: all install
all: $(OUT)/w202-bt-init

$(OUT):
	mkdir -p "$@"

$(OUT)/w202-bt-init: bt-init/w202-bt-init.c | $(OUT)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o "$@"

# Stage only. Enabling services, loading modules and provisioning the BD address belong to integration.
install: all
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	mkdir -p "$(DESTDIR)"
	cp -a rootfs/. "$(DESTDIR)/"
	install -m 0755 rootfs/usr/local/sbin/w202-bluetooth-start "$(DESTDIR)/usr/local/sbin/w202-bluetooth-start"
	install -m 0755 rootfs/usr/local/sbin/w202-wcn-reopen "$(DESTDIR)/usr/local/sbin/w202-wcn-reopen"
	install -m 0755 rootfs/usr/lib/systemd/system-sleep/w202-wcn "$(DESTDIR)/usr/lib/systemd/system-sleep/w202-wcn"
	install -D -m 0755 "$(OUT)/w202-bt-init" "$(DESTDIR)/usr/local/sbin/w202-bt-init"
	install -D -m 0644 $(FIRMWARE)/bt_configure_pskey.ini "$(DESTDIR)/usr/lib/firmware/bt_configure_pskey.ini"
	install -D -m 0644 $(FIRMWARE)/bt_configure_rf.ini "$(DESTDIR)/usr/lib/firmware/bt_configure_rf.ini"
