// SPDX-License-Identifier: GPL-2.0-only
/* Native H.264 IDR hardware test. Parses SPS/PPS/slice syntax in software,
 * passes standard V4L2 H.264 controls to the shared kernel parameter builder,
 * and executes its register list. No factory library or saved job is used.
 * Restricted to the builder's documented experimental subset.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <drm/panfrost_drm.h>
#include "../../source/w202_vpu_buffer_uapi.h"
#include "h264-idr-fixture.h"

struct native_buffer { int fd; void *cpu; size_t size; uint64_t address; };
static int decoder = -1;
static volatile uint32_t *registers;
static void check(bool ok, const char *message)
{
	if (ok) return;
	fprintf(stderr, "Native IDR error: %s (errno=%d)\n", message, errno);
	if (registers) munmap((void *)registers, 0x14000);
	if (decoder >= 0) close(decoder); /* resets or quarantines imported DMA */
	exit(1);
}
static void cpu_access(struct native_buffer *b, uint64_t flags)
{
	struct dma_buf_sync sync = {.flags = flags};
	check(!ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync), "CPU synchronization");
}
static struct native_buffer allocate(size_t size, const void *initial, size_t bytes)
{
	struct native_buffer b = {.size = (size + 4095) & ~(size_t)4095};
	int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	check(render >= 0, "Panfrost device");
	struct drm_panfrost_create_bo bo = {.size = b.size};
	check(!ioctl(render, DRM_IOCTL_PANFROST_CREATE_BO, &bo), "allocate BO");
	struct drm_prime_handle prime = {.handle = bo.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
	check(!ioctl(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "export BO");
	close(render); b.fd = prime.fd;
	b.cpu = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
	check(b.cpu != MAP_FAILED, "map BO");
	cpu_access(&b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	memset(b.cpu, 0, b.size);
	if (initial) { check(bytes <= b.size, "initial byte count"); memcpy(b.cpu, initial, bytes); }
	cpu_access(&b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	struct w202_vpu_iova map = {.fd = b.fd, .need_cache_sync = 1};
	check(!ioctl(decoder, W202_VPU_GET_IOVA, &map) && map.size == b.size, "import BO");
	b.address = map.iova_addr;
	return b;
}
static void reg_write(unsigned int offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	registers[offset / 4] = value;
}
static uint32_t reg_read(unsigned int offset)
{
	uint32_t value = registers[offset / 4];
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}
static void poll(unsigned int offset, uint32_t mask, uint32_t expected)
{
	struct timespec start, now;
	check(!clock_gettime(CLOCK_MONOTONIC, &start), "clock");
	while ((reg_read(offset) & mask) != expected) {
		check(!clock_gettime(CLOCK_MONOTONIC, &now), "clock");
		int64_t ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec;
		check(ns < 20000000, "BSM timeout");
	}
}
static void write_all(int fd, const void *data, size_t length)
{
	const char *p = data;
	while (length) {
		ssize_t n = write(fd, p, length);
		if (n < 0 && errno == EINTR) continue;
		check(n > 0, "write output"); p += n; length -= (size_t)n;
	}
}
int main(int argc, char **argv)
{
	check(argc == 4, "usage: w202-vpu-native-idr INPUT.h264 OUTPUT.nv12 CONFIG.bin");
	FILE *source = fopen(argv[1], "rb");
	check(source && !fseek(source, 0, SEEK_END), "open input");
	long length = ftell(source);
	check(length > 0 && length <= 8 * 1024 * 1024 && !fseek(source, 0, SEEK_SET), "input length");
	unsigned char *stream = malloc((size_t)length);
	check(stream && fread(stream, 1, (size_t)length, source) == (size_t)length, "read input");
	fclose(source);
	struct h264_fixture f;
	check(!h264_parse_idr(stream, (size_t)length, &f), "supported single IDR syntax");
	unsigned int width = (f.sps.pic_width_in_mbs_minus1 + 1) * 16;
	unsigned int height = (f.sps.pic_height_in_map_units_minus1 + 1) * 16;
	struct w202_h264_job job = {
		.sps = &f.sps, .pps = &f.pps, .slice = &f.slice, .decode = &f.decode,
		.scaling = &f.scaling,
		.input_address = 0x40000000, .output_address = 0x41000000, .auxiliary_address = 0x42000000,
		.input_capacity = f.nal_size + W202_H264_BSM_PADDING,
		.output_capacity = (uint64_t)width * height * 3 / 2,
		.auxiliary_capacity = (uint64_t)(width / 16) * (height / 16) * 80 + 1024,
		.input_bytes = f.nal_size, .nalu_bytes = f.rbsp_size,
	};
	struct w202_h264_config config;
	check(!w202_h264_build_idr(&job, &config), "supported native controls");
	int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(output >= 0, "new pixel output file");
	int config_fd = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(config_fd >= 0, "new register evidence file");
	decoder = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
	check(decoder >= 0, "exclusive decoder");
	check(!ioctl(decoder, _IO('m', 5), NULL) && !ioctl(decoder, _IO('m', 3), NULL) &&
	      !ioctl(decoder, _IO('m', 8), NULL), "acquire/enable/reset");
	struct native_buffer input = allocate(job.input_capacity, f.nal, f.nal_size);
	struct native_buffer frame = allocate(job.output_capacity, NULL, 0);
	struct native_buffer auxiliary = allocate(job.auxiliary_capacity, NULL, 0);
	job.input_address = input.address; job.input_capacity = input.size;
	job.output_address = frame.address; job.output_capacity = frame.size;
	job.auxiliary_address = auxiliary.address; job.auxiliary_capacity = auxiliary.size;
	check(!w202_h264_build_idr(&job, &config), "native hardware configuration");
	write_all(config_fd, &config, sizeof(config)); check(!close(config_fd), "close configuration");
	registers = mmap(NULL, 0x14000, PROT_READ | PROT_WRITE, MAP_SHARED, decoder, 0);
	if (registers == MAP_FAILED) { registers = NULL; check(false, "map decoder"); }
	reg_write(0x1010, 0xd0); reg_write(0x1028, 0); reg_write(0x1020, 4);
	poll(0x8018, 0x08000000, 0);
	reg_write(0x1080, config.stream_address >> 4); reg_write(0x1084, config.stream_address >> 4);
	reg_write(0x8008, 6); reg_write(0x8004, 0x80000000);
	reg_write(0x8000, (config.stream_bytes + W202_H264_BSM_PADDING) | 0x80000000);
	for (unsigned int bits = config.header_bits; bits;) {
		unsigned int n = bits > 32 ? 32 : bits;
		poll(0x8020, 1, 1); reg_write(0x8008, n << 24); reg_write(0x8008, (n << 24) | 1);
		(void)reg_read(0x8010); bits -= n;
	}
	for (unsigned int i = 0; i < config.count; i++) reg_write(config.regs[i].offset, config.regs[i].value);
	reg_write(0x1008, 0x7ff); reg_write(0x1004, 0x1035); reg_write(0xa0, 0xfb);
	reg_write(0x1038, 0xffffffff); reg_write(0x1028, 1); reg_write(0x1030, 10);
	uint32_t status = 0;
	int result = ioctl(decoder, _IO('m', 7), &status);
	fprintf(stderr, "Native IDR: coded=%ux%u visible=%ux%u crop=%u,%u qp=%d bits=%u EBSP=%zu RBSP=%zu irq=%08x wait=%d\n",
	        width, height, f.width, f.height, f.crop_left, f.crop_top,
	        26 + f.pps.pic_init_qp_minus26 + f.slice.slice_qp_delta, config.header_bits,
	        f.nal_size, f.rbsp_size, status, result);
	check(!result && (status & 4) && !(status & 0xffff0031), "hardware completion");
	check(!ioctl(decoder, _IO('m', 4), NULL), "stop/reset");
	cpu_access(&frame, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	const unsigned char *y = (unsigned char *)frame.cpu + f.crop_top * width + f.crop_left;
	const unsigned char *uv = (unsigned char *)frame.cpu + width * height + (f.crop_top / 2) * width + f.crop_left;
	for (unsigned int row = 0; row < f.height; row++) write_all(output, y + row * width, f.width);
	for (unsigned int row = 0; row < f.height / 2; row++) write_all(output, uv + row * width, f.width);
	cpu_access(&frame, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	check(!close(output), "close pixels");
	munmap((void *)registers, 0x14000); registers = NULL;
	check(!close(decoder), "close decoder"); decoder = -1;
	struct native_buffer *all[] = {&input, &frame, &auxiliary};
	for (unsigned int i = 0; i < 3; i++) { munmap(all[i]->cpu, all[i]->size); close(all[i]->fd); }
	free(stream);
	return 0;
}
