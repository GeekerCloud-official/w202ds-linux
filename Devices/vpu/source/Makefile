# Build locally against a matching kernel output.
ccflags-y += -I$(src)

obj-m += w202_vpu_power.o
obj-m += w202_vpu_iommu.o
obj-m += w202_vpu_codec.o
w202_vpu_codec-y := w202_vpu_buffers.o w202_vpu_ranges.o w202_vpu_codec_main.o w202_vpu_h264.o w202_vpu_video.o

# Optional diagnostics share the same headers and preserve their module names.
obj-m += w202_vpu_probe.o
w202_vpu_probe-y := tests/w202_vpu_probe.o
obj-m += w202_vpu_dma_test.o
w202_vpu_dma_test-y := tests/w202_vpu_dma_test.o
obj-m += w202_vpu_map_test.o
w202_vpu_map_test-y := w202_vpu_buffers.o w202_vpu_ranges.o tests/w202_vpu_map_test_main.o
