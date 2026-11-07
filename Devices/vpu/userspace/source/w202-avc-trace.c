// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in factory ABI observer; never linked into the installed runtime.
 * Build with w202-avc-worker.c, -DW202_AVC_TRACE and --export-dynamic.
 * Only known configuration/table registers are read. No target code or GOT is
 * patched, no register is written by this observer. Keep traces private: they
 * contain stream bytes and process addresses. All original calls still run.
 */
#include <stdint.h>
#include <stddef.h>
#include <asm-generic/ioctl.h>
#include <linux/dma-buf.h>

extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int ioctl(int, unsigned long, ...);
extern void *dlsym(void *, const char *);
extern void *memcpy(void *, const void *, size_t);
extern char *getenv(const char *);
extern int *__errno(void);
extern void exit(int) __attribute__((noreturn));

/* On-disk little-endian records: magic, type, a, b, payload length, reserved.
 * Types: 1 allocation (u64 IOVA,size), 2 BSM before (u32 fields),
 * 3 BSM after (u32 status), 4 bit read (a=count,b=value),
 * 5 selected registers (a=stage,b=job; u32 offset,value pairs),
 * 6 state block (a=kind; raw bytes), 7 buffer bytes (a=allocation index),
 * 8 table transfer (a=MMIO offset,b=16-byte groups), 9 IRQ result,
 * 10 header result (a=NAL type,b=library result: 2=new,3=continued slice).
 */
static int trace_fd = -1;
static unsigned int jobs;
static struct { int fd; void *cpu; size_t size; uint64_t iova; } buffers[96];
static unsigned int buffer_count;
static uint32_t (*original_bits)(uintptr_t *, unsigned int);
static int (*original_bsm)(uintptr_t *, int);
static uint64_t (*original_header)(uintptr_t *);
static uint32_t (*original_poll)(uintptr_t *);
static void (*original_table)(uintptr_t *, void *, int, int);

static void output(const void *data, size_t size)
{
	const char *p = data;
	while (size) {
		long n = write(trace_fd, p, size);
		if (n < 0 && *__errno() == 4) continue;
		if (n <= 0) exit(2);
		p += n; size -= (size_t)n;
	}
}

static void record(uint32_t type, uint32_t a, uint32_t b, const void *data, size_t size)
{
	if (trace_fd < 0) return;
	if (size > 32U * 1024 * 1024) exit(2);
	uint32_t header[] = {0x32555056, type, a, b, (uint32_t)size, 0};
	output(header, sizeof(header));
	if (size) output(data, size);
}

static uint32_t word(const void *p, size_t offset)
{
	uint32_t value;
	memcpy(&value, (const char *)p + offset, sizeof(value));
	return value;
}

static void snapshot(uintptr_t *vo, unsigned int stage)
{
	/* Never read BSM data/trigger registers, IRQ clear, MMU, or reserved MMIO. */
	static const unsigned int ranges[][2] = {
		{0x1010, 0x1014}, {0x1020, 0x1028}, {0x1034, 0x1038},
		{0x103c, 0x1094}, {0x1160, 0x119c},
		{0x8000, 0x8008}, {0x8018, 0x8024}, {0x802c, 0x8030}, {0x8034, 0x803c},
	};
	uint32_t values[2048];
	unsigned int n = 0;
	uintptr_t base = vo[0] + 0x61200000;
	for (unsigned int i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
		for (unsigned int offset = ranges[i][0]; offset < ranges[i][1]; offset += 4) {
			values[n++] = offset;
			values[n++] = *(volatile uint32_t *)(base + offset);
		}
	record(5, stage, jobs, values, n * sizeof(values[0]));
}

static void state(uintptr_t *vo)
{
	static const struct { unsigned int slot, size, kind; } blocks[] = {
		{0x37, 0x580, 1}, {0x38, 0x1b8, 2}, {99, 0x100, 3},
		{100, 0x148, 4}, {0x69, 0xc0, 5},
	};
	for (unsigned int i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
		if (vo[blocks[i].slot])
			record(6, blocks[i].kind, jobs, (void *)vo[blocks[i].slot], blocks[i].size);
	record(6, 6, jobs, (char *)vo + 0x16768, 0x240);
	record(6, 7, jobs, (char *)vo + 0x350, 0x20);
	record(6, 8, jobs, (char *)vo + 0x72b0, 0x40);
}

void w202_avc_trace_init(void *library)
{
	const char *path = getenv("W202_VPU_TRACE");
	if (!path || !*path) exit(2);
	trace_fd = open(path, 1 | 0100 | 0200 | 02000000, 0600); /* exclusive new file */
	if (trace_fd < 0) exit(2);
	original_bits = dlsym(library, "read_bits");
	original_bsm = dlsym(library, "H264Dec_cfg_bsm");
	original_header = dlsym(library, "H264Dec_Read_SPS_PPS_SliceHeader");
	original_poll = dlsym(library, "VSP_POLL_COMPLETE");
	original_table = dlsym(library, "VSP_TABLE_TRANSFER");
	if (!original_bits || !original_bsm || !original_header || !original_poll || !original_table)
		exit(2);
}

void w202_avc_trace_buffer(int fd, void *cpu, size_t size, uint64_t iova)
{
	if (buffer_count == 96) exit(2);
	unsigned int index = buffer_count++;
	buffers[index].fd = fd; buffers[index].cpu = cpu;
	buffers[index].size = size; buffers[index].iova = iova;
	uint64_t data[] = {iova, size};
	record(1, index, 0, data, sizeof(data));
}

uint32_t read_bits(uintptr_t *vo, unsigned int count)
{
	uint32_t value = original_bits(vo, count);
	if (jobs < 16) record(4, count, value, NULL, 0);
	return value;
}

int H264Dec_cfg_bsm(uintptr_t *vo, int search)
{
	if (jobs < 16) {
		uint32_t fields[] = {word(vo, 0x7360), word(vo, 0x736c), word(vo, 0x7370)};
		record(2, search, jobs, fields, sizeof(fields));
	}
	int result = original_bsm(vo, search);
	if (jobs < 16) {
		record(3, search, (uint32_t)result, NULL, 0);
		snapshot(vo, 1);
	}
	return result;
}

uint64_t H264Dec_Read_SPS_PPS_SliceHeader(uintptr_t *vo)
{
	uint64_t result = original_header(vo);
	if (jobs < 16) {
		snapshot(vo, 2);
		/* Non-slice NALs need not have an initialized picture. */
		unsigned int nal = *(unsigned char *)(vo[0x130] + 8);
		record(10, nal, (uint32_t)result, NULL, 0);
		if ((result == 2 || result == 3) && (nal == 1 || nal == 5)) {
			state(vo);
			if (!jobs) for (unsigned int i = 0; i < buffer_count; i++) {
				struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
				if (ioctl(buffers[i].fd, DMA_BUF_IOCTL_SYNC, &sync)) exit(2);
				record(7, i, 0, buffers[i].cpu, buffers[i].size);
				sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
				if (ioctl(buffers[i].fd, DMA_BUF_IOCTL_SYNC, &sync)) exit(2);
			}
		}
	}
	return result;
}

void VSP_TABLE_TRANSFER(uintptr_t *vo, void *data, int offset, int groups)
{
	if (jobs < 16 && groups >= 0 && groups <= 1024)
		record(8, (uint32_t)offset, (uint32_t)groups, data, (size_t)groups * 16);
	original_table(vo, data, offset, groups);
}

uint32_t VSP_POLL_COMPLETE(uintptr_t *vo)
{
	if (jobs < 16) snapshot(vo, 3);
	uint32_t result = original_poll(vo);
	if (jobs < 16) record(9, jobs, result, NULL, 0);
	jobs++;
	return result;
}
