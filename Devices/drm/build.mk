$(eval $(call module,w202ds-drm-panel,drm/source/baseline,w202_r6p0.ko))
$(eval $(call module,w202ds-drm-snapshot,drm/source/baseline,w202_dsi_snapshot.ko))
.PHONY: w202ds-drm-driver
w202ds-drm-driver: w202ds-drm-panel
SERVER_MODULES += w202ds-drm-panel
# DRM has no source, header, symbol or build dependency on boot handoff.

$(eval $(call module,w202ds-drm-backlight,drm/source/backlight,w202ds-backlight.ko,,CONFIG_W202DS_BACKLIGHT=m))
$(eval $(call module,w202ds-drm-handoff,drm/source/pj4edk2-handoff,pj4edk2-handoff.ko))
