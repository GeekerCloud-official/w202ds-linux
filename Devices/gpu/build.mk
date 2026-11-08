$(eval $(call module,w202ds-gpu-support,gpu/source/panfrost,w202_gpu_support.ko,,CONFIG_DRM_PANFROST=m))
$(eval $(call module,w202ds-gpu-panfrost,gpu/source/panfrost,panfrost.ko,w202ds-gpu-support,CONFIG_DRM_PANFROST=m))
.PHONY: w202ds-gpu-driver
w202ds-gpu-driver: w202ds-gpu-panfrost
SERVER_MODULES += w202ds-gpu-panfrost
# PanVK is a userspace Mesa project; its repository is not a kernel-module target.
