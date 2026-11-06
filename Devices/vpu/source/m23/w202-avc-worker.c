// SPDX-License-Identifier: GPL-2.0-only
/* Streaming Bionic worker for this board's measured factory AVC ABI.
 * stdin: Annex B with an AUD before every access unit (Linux FFmpeg BSF).
 * stdout: standard planar 4:2:0 Y4M; diagnostics, including vendor stdout,
 * go to stderr. Decoded pixels always come from actual VPU DMA-BUFs.
 */
#include <stddef.h>
#include <stdint.h>
#include <asm-generic/ioctl.h>
#include <linux/dma-buf.h>
#include <drm/panfrost_drm.h>
#include "w202_vpu_buffer_uapi.h"

extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern int fflush(void *);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *);
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern int dup2(int, int);
extern long read(int, void *, size_t);
extern long write(int, const void *, size_t);
extern int ioctl(int, unsigned long, ...);
extern void *mmap(void *, size_t, int, int, int, long);
extern int munmap(void *, size_t);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);
extern int dlclose(void *);
extern void exit(int) __attribute__((noreturn));
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *)
	__attribute__((noreturn));

#define O_RDWR 2
#define O_CLOEXEC 02000000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
#define FRAME_COUNT 24
#define MAX_BUFFERS 96
#define MAX_ACCESS_UNIT (8U * 1024 * 1024)
#define PADDING 4096

struct decoder_handle {
	void *state, *cookie;
	int (*bind)(void *, void *);
	int (*unbind)(void *, void *);
	int (*extra)(void *, unsigned int);
	int (*mbinfo)(void *, unsigned int, uint64_t *);
};
struct buffer {
	int fd;
	void *cpu;
	size_t size;
	struct w202_vpu_iova map;
	unsigned int refs;
};
struct decoder {
	struct decoder_handle h;
	void *library;
	int (*init)(void *, void *, void *, void *);
	int (*decode)(void *, void *, void *);
	int (*mem_init)(void *, void *);
	void (*set_picture)(void *, void *, uint64_t, void *, unsigned int, int);
	int (*release)(void *);
	int (*last)(void *, void **, unsigned int *, uint64_t *);
	int (*get_info)(void *, void *);
	struct buffer buffers[MAX_BUFFERS];
	struct buffer *frames[FRAME_COUNT], *current;
	unsigned int count, width, height, fps_n, fps_d, outputs, calls, units;
	unsigned int binds, unbinds, peak_refs;
	int fd, output;
};

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("W202_AVC_ERROR %s errno=%d\n", what, *__errno());
		fflush(NULL);
		exit(1); /* Kernel close resets before releasing any imported buffers. */
	}
}
static unsigned int number(const char *text)
{
	unsigned int n = 0;
	check(*text != 0, "empty numeric parameter");
	for (; *text; text++) {
		check(*text >= '0' && *text <= '9' && n <= 100000, "numeric parameter range");
		n = n * 10 + *text - '0';
	}
	return n;
}
static void put32(void *p, size_t offset, uint32_t value)
{
	memcpy((char *)p + offset, &value, sizeof(value));
}
static void put64(void *p, size_t offset, uint64_t value)
{
	memcpy((char *)p + offset, &value, sizeof(value));
}
static uint32_t get32(const void *p, size_t offset)
{
	uint32_t value;
	memcpy(&value, (const char *)p + offset, sizeof(value));
	return value;
}
static void *symbol(void *library, const char *name)
{
	void *value = dlsym(library, name);
	if (!value) printf("dlsym %s: %s\n", name, dlerror());
	check(value != NULL, "factory symbol");
	return value;
}
static void write_all(int fd, const void *data, size_t length)
{
	const char *p = data;
	while (length) {
		long n = write(fd, p, length);
		if (n < 0 && *__errno() == 4) continue;
		check(n > 0, "output write");
		p += n;
		length -= n;
	}
}
static void sync_buffer(struct buffer *b, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };
	check(!ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync), "DMA-BUF CPU synchronization");
}
static struct buffer *allocate(struct decoder *d, size_t size)
{
	struct drm_panfrost_create_bo bo = { .size = (size + 4095) & ~(size_t)4095 };
	struct drm_prime_handle prime = { .flags = DRM_CLOEXEC | DRM_RDWR };
	struct buffer *b;
	int render;
	check(d->count < MAX_BUFFERS && size && size <= 32 * 1024 * 1024, "buffer size/count");
	render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	check(render >= 0, "Panfrost render device");
	check(!ioctl(render, DRM_IOCTL_PANFROST_CREATE_BO, &bo), "GPU buffer allocation");
	prime.handle = bo.handle;
	check(!ioctl(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "PRIME export");
	close(render);
	b = &d->buffers[d->count++];
	b->fd = prime.fd;
	b->size = (size_t)bo.size;
	b->cpu = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
	check(b->cpu != MAP_FAILED, "DMA-BUF mmap");
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	memset(b->cpu, 0, b->size);
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	b->map.fd = b->fd;
	b->map.need_cache_sync = 1;
	check(!ioctl(d->fd, W202_VPU_GET_IOVA, &b->map), "VPU DMA-BUF import");
	check(b->map.size == b->size && b->map.iova_addr >= 0x20000000 &&
	      b->map.iova_addr + b->size <= 0x60000000, "VPU IOVA span");
	return b;
}
static struct buffer *frame_header(struct decoder *d, void *header)
{
	for (unsigned int i = 0; i < FRAME_COUNT; i++)
		if (header == d->frames[i]) return d->frames[i];
	check(0, "frame header outside live frame pool");
	return NULL;
}
static int bind_frame(void *cookie, void *header)
{
	struct decoder *d = cookie;
	struct buffer *b = frame_header(d, header);
	check(b->refs < 32, "reference count overflow");
	b->refs++;
	d->binds++;
	unsigned int refs = 0;
	for (unsigned int i = 0; i < FRAME_COUNT; i++) refs += d->frames[i]->refs;
	if (refs > d->peak_refs) d->peak_refs = refs;
	return 0;
}
static int unbind_frame(void *cookie, void *header)
{
	struct decoder *d = cookie;
	struct buffer *b = frame_header(d, header);
	check(b->refs > 0, "reference count underflow");
	b->refs--;
	d->unbinds++;
	return 0;
}
static int extra(void *cookie, unsigned int size)
{
	struct decoder *d = cookie;
	uint64_t descriptors[16] = {0};
	struct buffer *b = allocate(d, (size_t)size + PADDING);
	put64(descriptors, 0x28, (uint64_t)(uintptr_t)b->cpu);
	put64(descriptors, 0x30, b->map.iova_addr);
	put32(descriptors, 0x38, b->size);
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	int ret = d->mem_init(&d->h, descriptors);
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	return ret;
}
static int mbinfo(void *cookie, unsigned int size, uint64_t *device_address)
{
	struct decoder *d = cookie;
	/* This callback allocates an internal DPB slot's metadata, not a client
	 * frame's metadata. Stock init_picture caches this address in that slot
	 * and invokes us only when the slot has none. Keep each allocation until
	 * release; sharing it by reconstruction-frame identity corrupts later DPB
	 * reuse. The total remains bounded by the library's live DPB slot pool.
	 */
	struct buffer *b = allocate(d, size);
	*device_address = b->map.iova_addr;
	return 0;
}
static struct buffer *available_frame(struct decoder *d)
{
	for (unsigned int i = 0; i < FRAME_COUNT; i++)
		if (!d->frames[i]->refs) return d->frames[i];
	check(0, "decoder has retained the entire frame pool");
	return NULL;
}
static void save_frame(struct decoder *d, struct buffer *b)
{
	uint32_t info[32] = {0};
	unsigned char chroma[1920];
	check(!d->get_info(&d->h, info), "sequence dimensions");
	unsigned int stride = info[1], rows = info[2];
	unsigned int left = info[8], top = info[10], width = info[9], height = info[11];
	check(width == d->width && height == d->height && stride >= width && rows >= height &&
	      left <= stride - width && top <= rows - height && !((left | top) & 1) && !info[14],
	      "constant progressive 4:2:0 output crop");
	check((size_t)stride * rows * 3 / 2 <= b->size, "coded frame fits retained DMA-BUF");
	if (!d->outputs) {
		char header[160];
		int n = snprintf(header, sizeof(header),
			"YUV4MPEG2 W%u H%u F%u:%u Ip A0:0 C420jpeg\n",
			width, height, d->fps_n, d->fps_d);
		check(n > 0 && (size_t)n < sizeof(header), "Y4M header size");
		write_all(d->output, header, n);
		printf("W202_AVC_LAYOUT coded=%ux%u crop=%u,%u,%u,%u\n", stride, rows, left, top, width, height);
	}
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	write_all(d->output, "FRAME\n", 6);
	const unsigned char *y = (const unsigned char *)b->cpu + (size_t)top * stride + left;
	const unsigned char *uv = (const unsigned char *)b->cpu + (size_t)stride * rows + (size_t)(top / 2) * stride + left;
	for (unsigned int row = 0; row < height; row++) write_all(d->output, y + (size_t)row * stride, width);
	for (unsigned int plane = 0; plane < 2; plane++)
		for (unsigned int row = 0; row < height / 2; row++) {
			for (unsigned int x = 0; x < width / 2; x++) chroma[x] = uv[(size_t)row * stride + x * 2 + plane];
			write_all(d->output, chroma, width / 2);
		}
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	d->outputs++;
}

/* Each packet is delimited by AUD NALs inserted by the Linux BSF. Retain bytes
 * across 64 KiB reads, including a start code that straddles a read boundary.
 * The memory bound is per access unit; total stream duration is unbounded.
 */
static size_t next_unit(unsigned char *source, size_t *used, int *eof)
{
	size_t scan = 0;
	unsigned int aud = 0;
	for (;;) {
		while (scan + 4 <= *used) {
			size_t prefix = 0;
			if (scan + 5 <= *used && !source[scan] && !source[scan + 1] &&
			    !source[scan + 2] && source[scan + 3] == 1) prefix = 4;
			else if (!source[scan] && !source[scan + 1] && source[scan + 2] == 1) prefix = 3;
			if (prefix) {
				if ((source[scan + prefix] & 31) == 9 && ++aud == 2) return scan;
				scan += prefix + 1;
			} else scan++;
		}
		if (*eof) {
			check(!*used || aud == 1, "AUD-delimited Annex B stream");
			return *used;
		}
		check(*used < MAX_ACCESS_UNIT, "access unit exceeds 8 MiB");
		size_t capacity = MAX_ACCESS_UNIT - *used;
		if (capacity > 65536) capacity = 65536;
		long n = read(0, source + *used, capacity);
		if (n < 0 && *__errno() == 4) continue;
		check(n >= 0, "read Annex B input");
		if (!n) *eof = 1;
		else *used += n;
	}
}

int main(int argc, char **argv, char **env)
{
	(void)env;
	struct decoder d = { .fd = -1 };
	d.output = dup(1);
	check(d.output >= 3 && dup2(2, 1) == 1, "separate pixels and diagnostics");
	check(argc == 5, "usage: w202-avc-worker WIDTH HEIGHT FPS_NUM FPS_DEN");
	d.width = number(argv[1]); d.height = number(argv[2]);
	d.fps_n = number(argv[3]); d.fps_d = number(argv[4]);
	check(d.width >= 16 && d.width <= 1920 && d.height >= 16 && d.height <= 1080 &&
	      !((d.width | d.height) & 1) && d.fps_n && d.fps_d && d.fps_n <= 240 * d.fps_d,
	      "supported dimensions/framerate");
	uint64_t memory[16] = {0}, format[16] = {0}, config[2] = {0};
	void *internal = calloc(1, 2 * 1024 * 1024);
	unsigned char *source = calloc(1, MAX_ACCESS_UNIT + PADDING);
	check(internal && source, "decoder/stream memory");
	d.library = dlopen("/vendor/lib64/libomx_avcdec_hw_sprd.so", 2);
	if (!d.library) printf("dlopen: %s\n", dlerror());
	check(d.library != NULL, "factory AVC dependency closure");
	d.init = symbol(d.library, "H264DecInit");
	d.decode = symbol(d.library, "H264DecDecode");
	d.mem_init = symbol(d.library, "H264DecMemInit");
	d.set_picture = symbol(d.library, "H264Dec_SetCurRecPic");
	d.release = symbol(d.library, "H264DecRelease");
	d.last = symbol(d.library, "H264Dec_GetLastDspFrm");
	d.get_info = symbol(d.library, "H264DecGetInfo");
	d.h.cookie = &d; d.h.bind = bind_frame; d.h.unbind = unbind_frame;
	d.h.extra = extra; d.h.mbinfo = mbinfo;
	memory[0] = (uint64_t)(uintptr_t)internal; memory[2] = 2 * 1024 * 1024;
	check(!d.init(&d.h, memory, format, config) && d.h.state, "factory decoder initialization");
	d.fd = (int)get32(d.h.state, 8);
	check(d.fd > 0, "decoder owns its VPU fd");
	size_t frame_size = (size_t)((d.width + 15) & ~15U) * ((d.height + 15) & ~15U) * 3 / 2 + 65536;
	for (unsigned int i = 0; i < FRAME_COUNT; i++) d.frames[i] = allocate(&d, frame_size);
	struct buffer *stream = allocate(&d, MAX_ACCESS_UNIT + PADDING);
	size_t used = 0, length;
	int eof = 0;
	while ((length = next_unit(source, &used, &eof))) {
		size_t offset = 0;
		unsigned int calls = 0;
		while (offset < length) {
			uint64_t in[32] = {0}, out[32] = {0};
			check(++calls <= 1024 && d.calls != UINT32_MAX, "access-unit decode progress bound");
			d.current = available_frame(&d);
			d.set_picture(&d.h, d.current->cpu, d.current->map.iova_addr, d.current, d.calls, d.current->fd);
			sync_buffer(stream, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
			memcpy(stream->cpu, source + offset, length - offset);
			memset((char *)stream->cpu + length - offset, 0, PADDING);
			sync_buffer(stream, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
			put64(in, 0, (uint64_t)(uintptr_t)stream->cpu); put64(in, 8, stream->map.iova_addr);
			put32(in, 16, length - offset); put32(in, 20, 1);
			put64(in, 32, d.units);
			int ret = d.decode(&d.h, in, out);
			uint32_t consumed = get32(in, 16);
			d.calls++;
			check(ret == 0 || ret == -9, "factory accepted access unit");
			check(consumed && consumed <= length - offset, "bounded input consumption");
			if (get32(out, 48)) save_frame(&d, frame_header(&d, (void *)(uintptr_t)out[7]));
			offset += consumed;
		}
		d.units++;
		used -= length;
		memmove(source, source + length, used);
	}
	for (unsigned int i = 0; i <= FRAME_COUNT; i++) {
		void *header = NULL;
		unsigned int index = 0;
		uint64_t pts = 0;
		int ret = d.last(&d.h, &header, &index, &pts);
		if (ret || !header) break;
		check(i < FRAME_COUNT, "drain progress bound");
		save_frame(&d, frame_header(&d, header));
	}
	check(d.outputs > 0, "hardware produced video frames");
	check(!d.release(&d.h), "factory decoder release/reset");
	check(d.binds == d.unbinds, "all decoder frame references released");
	for (unsigned int i = 0; i < d.count; i++) {
		munmap(d.buffers[i].cpu, d.buffers[i].size);
		close(d.buffers[i].fd);
	}
	printf("W202_AVC_DONE units=%u frames=%u calls=%u buffers=%u binds=%u unbinds=%u peak_refs=%u\n",
	       d.units, d.outputs, d.calls, d.count, d.binds, d.unbinds, d.peak_refs);
	fflush(NULL);
	close(d.output);
	free(source); free(internal); dlclose(d.library);
	return 0;
}

__attribute__((used, noreturn)) static void start_main(void *raw)
{
	const void *structors[3] = {NULL, NULL, NULL};
	__libc_init(raw, NULL, main, structors);
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\n"
	"bti j\nmov x29,xzr\nmov x30,xzr\nmov x0,sp\nb start_main\n"
	".size _start, .-_start\n");
