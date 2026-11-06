$(eval $(call module,w202ds-touchscreen-himax,touchscreen/source/himax,himax_w202.ko,w202ds-drm-panel))
.PHONY: w202ds-touchscreen-driver
w202ds-touchscreen-driver: w202ds-touchscreen-himax
# Historical Linux module; explicit opt-in pending current Server validation.
