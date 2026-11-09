# SPDX-License-Identifier: MIT
OUT ?= output
MSGFMT ?= msgfmt
UUID := w202-power-menu@w202ds
DOMAIN := w202-power-menu
EXTDIR := /usr/share/gnome-shell/extensions/$(UUID)
LOCKDIR := /usr/share/gnome-shell/extensions/w202-screen-lock@w202ds
LANGS := $(basename $(notdir $(wildcard power-menu/po/*.po)))
MO := $(foreach l,$(LANGS),$(OUT)/locale/$(l)/LC_MESSAGES/$(DOMAIN).mo)
QTDIR := /usr/share/gnome-shell/extensions/w202-quick-toggles@w202ds
QTDOMAIN := w202-quick-toggles
QTLANGS := $(basename $(notdir $(wildcard quick-toggles/po/*.po)))
QTMO := $(foreach l,$(QTLANGS),$(OUT)/locale/$(l)/LC_MESSAGES/$(QTDOMAIN).mo)

.PHONY: all install
all: $(MO) $(QTMO)

$(OUT)/locale/%/LC_MESSAGES/$(DOMAIN).mo: power-menu/po/%.po
	mkdir -p "$(@D)"
	$(MSGFMT) --check -o "$@" $<

$(OUT)/locale/%/LC_MESSAGES/$(QTDOMAIN).mo: quick-toggles/po/%.po
	mkdir -p "$(@D)"
	$(MSGFMT) --check -o "$@" $<

# Stage only. Compiling the dconf database and enabling the service belong to integration.
install: all
	test -n "$(DESTDIR)" && test "$(DESTDIR)" != /
	mkdir -p "$(DESTDIR)"
	cp -a rootfs/. "$(DESTDIR)/"
	install -m 0755 rootfs/usr/local/sbin/w202-power-key "$(DESTDIR)/usr/local/sbin/w202-power-key"
	install -m 0755 rootfs/usr/local/bin/w202-screen-settings "$(DESTDIR)/usr/local/bin/w202-screen-settings"
	install -D -m 0644 power-menu/metadata.json "$(DESTDIR)$(EXTDIR)/metadata.json"
	install -D -m 0644 power-menu/extension.js "$(DESTDIR)$(EXTDIR)/extension.js"
	install -D -m 0644 screen-lock/metadata.json "$(DESTDIR)$(LOCKDIR)/metadata.json"
	install -D -m 0644 screen-lock/extension.js "$(DESTDIR)$(LOCKDIR)/extension.js"
	for l in $(LANGS); do \
		install -D -m 0644 "$(OUT)/locale/$$l/LC_MESSAGES/$(DOMAIN).mo" \
			"$(DESTDIR)$(EXTDIR)/locale/$$l/LC_MESSAGES/$(DOMAIN).mo" || exit 1; \
	done
	install -D -m 0644 quick-toggles/metadata.json "$(DESTDIR)$(QTDIR)/metadata.json"
	install -D -m 0644 quick-toggles/extension.js "$(DESTDIR)$(QTDIR)/extension.js"
	install -D -m 0644 quick-toggles/icons/w202-flashlight-symbolic.svg \
		"$(DESTDIR)$(QTDIR)/icons/w202-flashlight-symbolic.svg"
	for l in $(QTLANGS); do \
		install -D -m 0644 "$(OUT)/locale/$$l/LC_MESSAGES/$(QTDOMAIN).mo" \
			"$(DESTDIR)$(QTDIR)/locale/$$l/LC_MESSAGES/$(QTDOMAIN).mo" || exit 1; \
	done
