# Camera core/source dependencies are incomplete; no placeholder module target.
# The rear torch LED is independent of the camera pipeline.
$(eval $(call module,w202ds-camera-flash,camera/source/w202-flash,w202_flash_aw36518.ko))
