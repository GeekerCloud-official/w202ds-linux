// SPDX-License-Identifier: GPL-2.0-only
/* Privileged, single-IDR hardware experiment. Reconstructs one observed job
 * using fresh DMA-BUF mappings. Does not load or execute the factory library.
 * This is a controlled register replay, not a general H.264 decoder or UAPI.
 * Input: private trace from w202-avc-trace; HEADER_BITS includes the NAL byte
 * and is measured independently with FFmpeg trace_headers (RBSP bit count).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <drm/panfrost_drm.h>
#include "../../source/w202_vpu_buffer_uapi.h"

#define MMIO_SIZE 0x14000
#define CMD_ENABLE _IO('m', 3)
#define CMD_DISABLE _IO('m', 4)
#define CMD_ACQUIRE _IO('m', 5)
#define CMD_COMPLETE _IO('m', 7)
#define CMD_RESET _IO('m', 8)

struct buffer {
	uint64_t old_iova, iova;
	size_t size;
	const void *initial;
	void *cpu;
	int fd;
};
static struct buffer buffers[96];
static unsigned int count;
static int codec = -1;
static volatile uint32_t *mmio;

static void require(bool ok, const char *what)
{
	if (ok) return;
	fprintf(stderr, "IDR replay: %s: %s\n", what, strerror(errno));
	if (codec >= 0) {
		/* Drop the MMIO VMA before closing: it retains the codec file. The
		 * driver resets or quarantines all imports before releasing DMA. */
		if (mmio) { munmap((void *)mmio, MMIO_SIZE); mmio = NULL; }
		close(codec);
	}
	exit(1);
}

static uint32_t u32(const void *p)
{
	uint32_t v; memcpy(&v, p, sizeof(v)); return v;
}
static uint64_t u64(const void *p)
{
	uint64_t v; memcpy(&v, p, sizeof(v)); return v;
}
static void sync_buffer(struct buffer *b, uint64_t flags)
{
	struct dma_buf_sync sync = {.flags = flags};
	require(!ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync), "DMA-BUF synchronization");
}
static void allocate(struct buffer *b)
{
	int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	require(render >= 0, "render device");
	struct drm_panfrost_create_bo bo = {.size = b->size};
	require(!ioctl(render, DRM_IOCTL_PANFROST_CREATE_BO, &bo), "allocate BO");
	struct drm_prime_handle prime = {.handle = bo.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
	require(!ioctl(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "export BO");
	close(render);
	b->fd = prime.fd;
	b->cpu = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
	require(b->cpu != MAP_FAILED, "mmap BO");
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	memcpy(b->cpu, b->initial, b->size);
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	struct w202_vpu_iova map = {.fd = b->fd, .need_cache_sync = 1};
	require(!ioctl(codec, W202_VPU_GET_IOVA, &map), "import BO");
	require(map.size == b->size && !(map.iova_addr & 15), "import span");
	b->iova = map.iova_addr;
}
static struct buffer *lookup(uint64_t old, size_t length)
{
	for (unsigned int i = 0; i < count; i++)
		if (old >= buffers[i].old_iova && old - buffers[i].old_iova <= buffers[i].size &&
		    length <= buffers[i].size - (old - buffers[i].old_iova)) return &buffers[i];
	require(false, "address outside captured buffers");
	return NULL;
}
static uint32_t relocate(uint32_t address)
{
	if (!address) return 0;
	uint64_t old = (uint64_t)address << 4;
	struct buffer *b = lookup(old, 16);
	return (uint32_t)((b->iova + old - b->old_iova) >> 4);
}
static uint32_t read_reg(unsigned int offset)
{
	uint32_t value = mmio[offset / 4];
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}
static void write_reg(unsigned int offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	mmio[offset / 4] = value;
}
static void poll_reg(unsigned int offset, uint32_t mask, uint32_t expected)
{
	struct timespec start, now;
	require(!clock_gettime(CLOCK_MONOTONIC, &start), "monotonic clock");
	while ((read_reg(offset) & mask) != expected) {
		require(!clock_gettime(CLOCK_MONOTONIC, &now), "monotonic clock");
		int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec;
		require(elapsed < 20000000, "BSM poll timeout");
	}
}
static bool config_register(unsigned int r)
{
	return r == 0x1020 || r == 0x1024 || r == 0x1034 ||
	       (r >= 0x103c && r <= 0x1090) || (r >= 0x1160 && r <= 0x1198);
}

int main(int argc, char **argv)
{
	require(argc == 4, "usage: w202-vpu-idr-replay TRACE HEADER_BITS OUTPUT.nv12");
	char *end;
	unsigned long header_bits = strtoul(argv[2], &end, 10);
	require(*argv[2] && !*end && header_bits >= 8 && header_bits <= 4096, "header bit count");
	FILE *input = fopen(argv[1], "rb");
	require(input && !fseek(input, 0, SEEK_END), "open trace");
	long length = ftell(input);
	require(length > 0 && length <= 128 * 1024 * 1024 && !fseek(input, 0, SEEK_SET), "trace size");
	unsigned char *trace = malloc((size_t)length);
	require(trace && fread(trace, 1, (size_t)length, input) == (size_t)length, "read trace");
	fclose(input);
	uint32_t regs[MMIO_SIZE / 4] = {0};
	bool present[MMIO_SIZE / 4] = {0};
	uint32_t bsm_offset = 0, bsm_address = 0, bsm_length = 0;
	unsigned int irq_count = 0, slices = 0, snapshots = 0;
	const unsigned char *huffman = NULL;
	for (size_t pos = 0; pos < (size_t)length;) {
		require((size_t)length - pos >= 24 && u32(trace + pos) == 0x32555056, "trace header");
		uint32_t type = u32(trace + pos + 4), a = u32(trace + pos + 8);
		uint32_t b = u32(trace + pos + 12), size = u32(trace + pos + 16);
		require(size <= (size_t)length - pos - 24, "trace record size");
		const unsigned char *data = trace + pos + 24;
		if (type == 1) {
			require(a == count && count < 96 && size == 16, "buffer descriptor");
			struct buffer *v = &buffers[count++];
			v->old_iova = u64(data); v->size = (size_t)u64(data + 8);
			require(v->size && v->size <= 32 * 1024 * 1024 && !(v->size & 4095) &&
			        v->old_iova >= 0x20000000 && v->old_iova + v->size <= 0x60000000, "buffer bounds");
		} else if (type == 2 && !a) {
			require(size == 12, "BSM descriptor");
			bsm_offset = u32(data); bsm_address = u32(data + 4); bsm_length = u32(data + 8);
		} else if (type == 5 && a == 3) {
			snapshots++;
			require(!(size & 7), "register pair layout");
			for (unsigned int i = 0; i < size; i += 8) {
				uint32_t r = u32(data + i);
				require(!(r & 3) && r < MMIO_SIZE, "register offset");
				regs[r / 4] = u32(data + i + 4); present[r / 4] = true;
			}
		} else if (type == 7) {
			require(a < count && size == buffers[a].size && !buffers[a].initial, "buffer snapshot");
			buffers[a].initial = data;
		} else if (type == 8) {
			require(a == 0x3000 && b == 0x12 && size == 0x120 && !huffman, "CAVLC table transfer");
			huffman = data;
		} else if (type == 9) {
			irq_count++;
			require((b & 4) && !(b & 0xffff0031), "captured completion status");
		} else if (type == 10 && (a == 1 || a == 5)) {
			require(a == 5 && b == 2, "only one first IDR slice is supported");
			slices++;
		}
		pos += 24 + size;
	}
	require(snapshots == 1 && slices == 1 && irq_count == 1 && huffman && bsm_length &&
	        bsm_length <= 8 * 1024 * 1024 && bsm_offset < bsm_length, "single IDR capture");
	for (unsigned int i = 0; i < count; i++) require(buffers[i].initial != NULL, "missing initial DMA bytes");
	unsigned int width = regs[0x1180 / 4] & 0xffff;
	unsigned int height = (regs[0x1180 / 4] >> 20) * 16;
	require(width >= 16 && width <= 1920 && height >= 16 && height <= 1088 &&
	        !(width & 15) && regs[0x1020 / 4] == 0x11000004, "linear progressive CAVLC IDR mode");
	struct buffer *stream = lookup(bsm_address, (size_t)bsm_length + 1024);
	uint64_t output_address = (uint64_t)regs[0x1088 / 4] << 4;
	struct buffer *frame = lookup(output_address, (size_t)width * height * 3 / 2);
	require(((uint64_t)regs[0x108c / 4] << 4) == output_address + width * height, "NV12 plane layout");
	require(((const unsigned char *)stream->initial)[bsm_address - stream->old_iova + bsm_offset] == 0x65,
	        "captured IDR NAL header");
	codec = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
	require(codec >= 0, "exclusive decoder");
	require(!ioctl(codec, CMD_ACQUIRE, NULL) && !ioctl(codec, CMD_ENABLE, NULL) &&
	        !ioctl(codec, CMD_RESET, NULL), "acquire/enable/reset");
	for (unsigned int i = 0; i < count; i++) allocate(&buffers[i]);
	mmio = mmap(NULL, MMIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, codec, 0);
	if (mmio == MAP_FAILED) { mmio = NULL; require(false, "decoder mmap"); }
	write_reg(0x1010, 0xd0); write_reg(0x1028, 0); write_reg(0x1020, 4);
	poll_reg(0x8018, 0x08000000, 0);
	write_reg(0x1080, relocate(bsm_address >> 4));
	write_reg(0x1084, relocate(bsm_address >> 4));
	write_reg(0x8008, 6);
	write_reg(0x8004, bsm_offset | 0x80000000);
	write_reg(0x8000, (bsm_length + 1024) | 0x80000000);
	for (unsigned long bits = header_bits; bits;) {
		unsigned int n = bits > 32 ? 32 : (unsigned int)bits;
		poll_reg(0x8020, 1, 1);
		write_reg(0x8008, n << 24); write_reg(0x8008, (n << 24) | 1);
		(void)read_reg(0x8010); bits -= n;
	}
	for (unsigned int r = 0; r < MMIO_SIZE; r += 4) {
		if (!present[r / 4] || !config_register(r)) continue;
		uint32_t v = regs[r / 4];
		if (r >= 0x1080 && r <= 0x1090) v = relocate(v);
		write_reg(r, v);
	}
	/* The table banks do not have reliable MMIO readback. Replay the CPU
	 * bytes observed at VSP_TABLE_TRANSFER, never a register snapshot. This
	 * IDR has no references, weighting or custom scaling/CABAC tables. */
	for (unsigned int i = 0; i < 0x120; i += 4) write_reg(0x3000 + i, u32(huffman + i));
	for (unsigned int i = 0; i < 0x88; i += 4) {
		write_reg(0x1200 + i, 0); write_reg(0x2000 + i, 0);
	}
	write_reg(0x1008, 0x7ff); write_reg(0x1004, 0x1035);
	write_reg(0xa0, 0xfb); write_reg(0x1038, 0xffffffff);
	write_reg(0x1028, 1); write_reg(0x1030, 10);
	uint32_t status = 0;
	int result = ioctl(codec, CMD_COMPLETE, &status);
	fprintf(stderr, "IDR replay: %ux%u, header_bits=%lu, irq=0x%08x, wait=%d\n",
	        width, height, header_bits, status, result);
	require(!result && (status & 4) && !(status & 0xffff0031), "hardware completion");
	require(!ioctl(codec, CMD_DISABLE, NULL), "stop/reset before CPU access");
	sync_buffer(frame, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	int output = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(output >= 0, "exclusive output file");
	const char *pixels = (const char *)frame->cpu + output_address - frame->old_iova;
	size_t bytes = (size_t)width * height * 3 / 2;
	while (bytes) {
		ssize_t n = write(output, pixels, bytes);
		if (n < 0 && errno == EINTR) continue;
		require(n > 0, "write output pixels"); pixels += n; bytes -= (size_t)n;
	}
	require(!close(output), "close output");
	sync_buffer(frame, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	munmap((void *)mmio, MMIO_SIZE); mmio = NULL;
	require(!close(codec), "close decoder"); codec = -1;
	for (unsigned int i = 0; i < count; i++) { munmap(buffers[i].cpu, buffers[i].size); close(buffers[i].fd); }
	free(trace);
	return 0;
}
