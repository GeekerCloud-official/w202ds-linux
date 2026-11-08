$(eval $(call module,w202ds-vpu-power,vpu/source,w202_vpu_power.ko))
$(eval $(call module,w202ds-vpu-iommu,vpu/source,w202_vpu_iommu.ko,w202ds-vpu-power))
$(eval $(call module,w202ds-vpu-m2m,$(abspath $(DEVICES)/../kernel/drivers/media/v4l2-core),v4l2-mem2mem.ko,,CONFIG_V4L2_MEM2MEM_DEV=m))
$(eval $(call module,w202ds-vpu-codec,vpu/source,w202_vpu_codec.ko,w202ds-vpu-iommu w202ds-vpu-m2m))
$(eval $(call module,w202ds-vpu-probe,vpu/source,w202_vpu_probe.ko,w202ds-vpu-power))
$(eval $(call module,w202ds-vpu-dma-test,vpu/source,w202_vpu_dma_test.ko,w202ds-vpu-iommu))
$(eval $(call module,w202ds-vpu-map-test,vpu/source,w202_vpu_map_test.ko,w202ds-vpu-iommu))
.PHONY: w202ds-vpu-driver
w202ds-vpu-driver: w202ds-vpu-codec
SERVER_MODULES += w202ds-vpu-codec
# Codec, map-test and DMA-test have conflicting hardware ownership at runtime.
