// SPDX-License-Identifier: GPL-2.0-only
/* Experimental native Baseline/Main IP hardware test. Parses SPS/PPS/slice syntax in software,
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
#ifdef W202_NATIVE_IPB
#include "h264-ipb-fixture.h"
typedef struct h264_ipb_fixture native_fixture;
#define NATIVE_FRAMES W202_IPB_FIXTURE_FRAMES
#define NATIVE_NAME "IPB"
#define parse_fixture h264_parse_ipb
#define build_fixture w202_h264_build_ipb
#else
#include "h264-ip-fixture.h"
typedef struct h264_ip_fixture native_fixture;
#define NATIVE_FRAMES W202_IP_FIXTURE_FRAMES
#define NATIVE_NAME "IP"
#define parse_fixture h264_parse_ip
#define build_fixture w202_h264_build_ip
#endif

struct native_buffer { int fd; void *cpu; size_t size; uint64_t address; };
static int decoder = -1;
static volatile uint32_t *registers;
static void check(bool ok, const char *message)
{
	if (ok) return;
	fprintf(stderr, "Native " NATIVE_NAME " error: %s (errno=%d)\n", message, errno);
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
 check(argc == 4, "usage: w202-vpu-native-ip[b] INPUT.h264 OUTPUT.nv12 CONFIG.bin");
 FILE *source = fopen(argv[1], "rb");
 check(source && !fseek(source, 0, SEEK_END), "open input");
 long bytes = ftell(source);
 check(bytes > 0 && bytes <= 8 * 1024 * 1024 && !fseek(source, 0, SEEK_SET), "input length");
 unsigned char *stream = malloc(bytes);
 check(stream && fread(stream, 1, bytes, source) == (size_t)bytes, "read input");
 fclose(source);
 native_fixture *fixture = malloc(sizeof(*fixture));
 check(fixture && !parse_fixture(stream, bytes, fixture), "supported " NATIVE_NAME " syntax");
 struct v4l2_ctrl_h264_scaling_matrix scaling;
 bool flat_matrix = getenv("W202_FLAT_MATRIX") != NULL;
 memset(&scaling, 16, sizeof(scaling));
 if (flat_matrix)
  for (unsigned int i = 0; i < fixture->count; i++)
   fixture->frame[i].pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
 unsigned int width = (fixture->frame[0].sps.pic_width_in_mbs_minus1 + 1) * 16;
 unsigned int height = (fixture->frame[0].sps.pic_height_in_map_units_minus1 + 1) * 16;
 size_t pixels = (size_t)width * height * 3 / 2;
 size_t aux = (width / 16) * (height / 16) * 80 + 1024, max_nal = 0;
 for (unsigned int i = 0; i < fixture->count; i++)
  if (max_nal < fixture->frame[i].nal_size) max_nal = fixture->frame[i].nal_size;
 int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
 check(output >= 0, "new output file");
 int config_fd = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
 check(config_fd >= 0, "new register file");
 decoder = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
 check(decoder >= 0, "exclusive decoder");
 check(!ioctl(decoder, _IO('m', 5), NULL) && !ioctl(decoder, _IO('m', 3), NULL) &&
       !ioctl(decoder, _IO('m', 8), NULL), "acquire/enable/reset");
 struct native_buffer input = allocate(max_nal + W202_H264_BSM_PADDING, NULL, 0);
 struct native_buffer frames[NATIVE_FRAMES];
 for (unsigned int i = 0; i < fixture->count; i++) frames[i] = allocate(pixels + aux, NULL, 0);
 registers = mmap(NULL, 0x14000, PROT_READ | PROT_WRITE, MAP_SHARED, decoder, 0);
 if (registers == MAP_FAILED) { registers = NULL; check(false, "map decoder"); }
 for (unsigned int i = 0; i < fixture->count; i++) {
  struct h264_fixture *f = &fixture->frame[i];
  struct native_buffer *frame = &frames[i];
  if (i) check(!ioctl(decoder, _IO('m', 3), NULL) && !ioctl(decoder, _IO('m', 8), NULL), "enable/reset");
  cpu_access(&input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
  memset(input.cpu, 0, input.size);
  memcpy(input.cpu, f->nal, f->nal_size);
  cpu_access(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
  struct w202_h264_job job = {
   .sps = &f->sps, .pps = &f->pps, .slice = &f->slice, .decode = &f->decode,
   .scaling = flat_matrix ? &scaling : &f->scaling,
   .input_address = input.address, .output_address = frame->address,
   .auxiliary_address = frame->address + pixels,
   .input_capacity = input.size, .output_capacity = pixels, .auxiliary_capacity = aux,
   .input_bytes = f->nal_size, .nalu_bytes = f->rbsp_size,
  };
  if (i) {
   unsigned int ref0 = i - 1;
#ifdef W202_NATIVE_IPB
   ref0 = fixture->ref0[i];
#endif
   check(ref0 < i, "decoded L0 reference");
   job.reference_address = frames[ref0].address;
   job.reference_auxiliary_address = frames[ref0].address + pixels;
   job.reference_capacity = pixels; job.reference_auxiliary_capacity = aux;
   job.reference_frame_id = ref0;
#ifdef W202_NATIVE_IPB
   if (f->slice.slice_type == V4L2_H264_SLICE_TYPE_B) {
    unsigned int ref1 = fixture->ref1[i];
    check(ref1 < i, "decoded L1 reference");
    job.reference1_address = frames[ref1].address;
    job.reference1_auxiliary_address = frames[ref1].address + pixels;
    job.reference1_capacity = pixels; job.reference1_auxiliary_capacity = aux;
    job.reference1_frame_id = ref1;
   }
#endif
  }
  struct w202_h264_config config = {0};
  int result = build_fixture(&job, &config);
  fprintf(stderr, "Native " NATIVE_NAME " setup: frame=%u type=%u frame_num=%u poc=%d qp=%d bits=%u bytes=%zu rbsp=%zu registers=%u build=%d profile=%u cabac=%u init=%u flat_matrix=%u\n",
          i, f->slice.slice_type, f->decode.frame_num, f->decode.top_field_order_cnt,
          26 + f->pps.pic_init_qp_minus26 + f->slice.slice_qp_delta,
          f->slice.header_bit_size, f->nal_size, f->rbsp_size, config.count, result,
          f->sps.profile_idc, !!(f->pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE),
          f->slice.cabac_init_idc, flat_matrix);
  check(!result, "native " NATIVE_NAME " configuration");
  write_all(config_fd, &config, sizeof(config));
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
  for (unsigned int j = 0; j < config.count; j++) reg_write(config.regs[j].offset, config.regs[j].value);
  reg_write(0x1008, 0x7ff); reg_write(0x1004, 0x1035); reg_write(0xa0, 0xfb);
  reg_write(0x1038, 0xffffffff); reg_write(0x1028, 1); reg_write(0x1030, 10);
  uint32_t status = 0;
  result = ioctl(decoder, _IO('m', 7), &status);
  fprintf(stderr, "Native " NATIVE_NAME " result: frame=%u irq=%08x wait=%d\n", i, status, result);
  check(!result && (status & 4) && !(status & 0xffffe031), "hardware completion");
  check(!ioctl(decoder, _IO('m', 4), NULL), "stop/reset");
 }
 for (unsigned int display = 0; display < fixture->count; display++) {
  unsigned int i = display;
#ifdef W202_NATIVE_IPB
  i = fixture->display_order[display];
#endif
  const struct h264_fixture *f = &fixture->frame[i];
  struct native_buffer *frame = &frames[i];
  fprintf(stderr, "Native " NATIVE_NAME " output: display=%u frame=%u poc=%d\n",
          display, i, f->decode.top_field_order_cnt);
  cpu_access(frame, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  const unsigned char *y = (unsigned char *)frame->cpu + f->crop_top * width + f->crop_left;
  const unsigned char *uv = (unsigned char *)frame->cpu + width * height + (f->crop_top / 2) * width + f->crop_left;
  for (unsigned int row = 0; row < f->height; row++) write_all(output, y + row * width, f->width);
  for (unsigned int row = 0; row < f->height / 2; row++) write_all(output, uv + row * width, f->width);
  cpu_access(frame, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
 }
 check(!close(config_fd) && !close(output), "close output files");
 munmap((void *)registers, 0x14000); registers = NULL;
 check(!close(decoder), "close decoder"); decoder = -1;
 munmap(input.cpu, input.size); close(input.fd);
 for (unsigned int i = 0; i < fixture->count; i++) { munmap(frames[i].cpu, frames[i].size); close(frames[i].fd); }
 free(fixture); free(stream);
 return 0;
}
