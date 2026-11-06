# Build only on CM2 against the pinned M23 output.
obj-m += w202_vpu_power.o
obj-m += w202_vpu_probe.o
obj-m += w202_vpu_iommu.o
obj-m += w202_vpu_dma_test.o
obj-m += w202_vpu_map_test.o
w202_vpu_map_test-y := w202_vpu_buffers.o w202_vpu_map_test_main.o
obj-m += w202_vpu_codec.o
w202_vpu_codec-y := w202_vpu_buffers.o w202_vpu_codec_main.o
