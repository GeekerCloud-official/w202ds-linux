# SPDX-License-Identifier: GPL-2.0-only
.PHONY: install

# Stage only. Installing the module, depmod, iio-sensor-proxy and enabling the service belong to integration.
install:
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	mkdir -p "$(DESTDIR)"
	cp -a rootfs/. "$(DESTDIR)/"
	install -m 0755 rootfs/usr/libexec/w202-sensorhub-attach "$(DESTDIR)/usr/libexec/w202-sensorhub-attach"
