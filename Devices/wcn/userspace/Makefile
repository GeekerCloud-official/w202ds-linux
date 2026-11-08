# SPDX-License-Identifier: MIT
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OUT ?= output
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static -s
FIRMWARE ?= ../firmware

.PHONY: all test install
all: $(OUT)/w202-bt-init

$(OUT)/w202-bt-init: bt-init/w202-bt-init.c
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o "$@"

test: all
	python3 tests/test-init.py "$(OUT)/w202-bt-init" "$(FIRMWARE)"

# Stage only: no module loads, service changes or device access.
install: all
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	mkdir -p "$(DESTDIR)"
	cp -a rootfs/. "$(DESTDIR)/"
	install -D -m 0755 "$(OUT)/w202-bt-init" "$(DESTDIR)/usr/libexec/w202-bt-init"
	install -D -m 0644 "$(FIRMWARE)/bt_configure_pskey.ini" "$(DESTDIR)/usr/lib/firmware/bt_configure_pskey.ini"
	install -D -m 0644 "$(FIRMWARE)/bt_configure_rf.ini" "$(DESTDIR)/usr/lib/firmware/bt_configure_rf.ini"
