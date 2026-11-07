// SPDX-License-Identifier: GPL-2.0-only
/* AArch64/Bionic test client for the measured factory H264 decoder ABI.
 * Compiled without a host libc or Android framework. Uses standard GPU PRIME
 * DMA-BUFs; no ION, raw physical allocation or fake successful codec output.
 */
#include <stddef.h>
#include <stdint.h>
#include <asm-generic/ioctl.h>
#include <linux/dma-buf.h>
#include <drm/panfrost_drm.h>
#include "../../source/w202_vpu_buffer_uapi.h"

extern int printf(const char *, ...);
extern int fflush(void *);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *);
extern int open(const char *, int, ...);
extern int close(int);
extern long read(int, void *, size_t);
extern long write(int, const void *, size_t);
extern long lseek(int, long, int);
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
#define O_WRONLY 1
#define O_CREAT 64
#define O_EXCL 128
#define O_CLOEXEC 02000000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
#define MAX_BUFFERS 48
#define FRAMES 16
#define FRAME_SIZE (1024 * 1024)

struct decoder_handle {
	void *state;
	void *cookie;
	int (*bind)(void *, void *);
	int (*unbind)(void *, void *);
	int (*extra)(void *, unsigned int);
	int (*mbinfo)(void *, unsigned int, uint64_t *);
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
	struct buffer {
		int fd;
		void *cpu;
		size_t size;
		struct w202_vpu_iova map;
	} buffers[MAX_BUFFERS];
	unsigned int count, width, height, outputs;
	int fd, output;
};

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL %s errno=%d\n", what, *__errno());
		exit(1); /* The kernel file release must quiesce any submitted DMA. */
	}
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
	if (!value) { printf("dlsym %s: %s\n", name, dlerror()); exit(1); }
	return value;
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
	int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct buffer *b;
	check(d->count < MAX_BUFFERS && size && size <= 32 * 1024 * 1024, "buffer size/count");
	check(render >= 0, "GPU render device");
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
	memset(b->cpu, 0xa5, b->size);
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	b->map.fd = b->fd;
	b->map.need_cache_sync = 1;
	check(!ioctl(d->fd, W202_VPU_GET_IOVA, &b->map), "VPU standard DMA-BUF import");
	check(b->map.size == b->size && b->map.iova_addr >= 0x20000000 &&
	      b->map.iova_addr + b->size <= 0x60000000, "VPU IOVA span");
	printf("DMA_BUFFER index=%u bytes=%lu iova=%#lx\n", d->count - 1,
	       (unsigned long)b->size, (unsigned long)b->map.iova_addr);
	return b;
}
static int binding(void *cookie, void *header)
{
	(void)cookie; (void)header;
	return 0; /* This bounded client retains every frame until decoder release. */
}
static int extra(void *cookie, unsigned int size)
{
	struct decoder *d = cookie;
	uint64_t descriptors[16] = {0};
	struct buffer *b = allocate(d, size + 4096);
	put64(descriptors, 0x28, (uint64_t)(uintptr_t)b->cpu);
	put64(descriptors, 0x30, b->map.iova_addr);
	put32(descriptors, 0x38, b->size);
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	int ret = d->mem_init(&d->h, descriptors);
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	printf("EXTRA_MEMORY bytes=%u result=%d\n", size, ret);
	return ret;
}
static int mbinfo(void *cookie, unsigned int size, uint64_t *device_address)
{
	struct decoder *d = cookie;
	struct buffer *b = allocate(d, size);
	*device_address = b->map.iova_addr;
	printf("MBINFO_MEMORY bytes=%u\n", size);
	return 0;
}
static void save_frame(struct decoder *d, struct buffer *b)
{
	size_t size = d->width * d->height * 3 / 2;
	check(size <= b->size, "output dimensions inside frame buffer");
	sync_buffer(b, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	check(write(d->output, b->cpu, size) == (long)size, "save real decoded frame");
	sync_buffer(b, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	printf("REAL_FRAME index=%u width=%u height=%u bytes=%lu first=%02x\n",
	       d->outputs++, d->width, d->height, (unsigned long)size,
	       *(unsigned char *)b->cpu);
}

int main(int argc, char **argv, char **env)
{
	(void)env;
	struct decoder d = { .fd = -1, .output = -1, .width = 64, .height = 64 };
	uint64_t memory[16] = {0}, format[16] = {0}, config[2] = {0};
	void *internal = calloc(1, 2 * 1024 * 1024);
	struct buffer *frames[FRAMES], *stream;
	int ret, init_only = argc == 2 && !strcmp(argv[1], "--init");
	check(init_only || argc == 3, "usage: test-vendor-avc --init | INPUT.h264 OUTPUT.raw");
	check(internal != NULL, "decoder internal memory");
	d.library = dlopen("/vendor/lib64/libomx_avcdec_hw_sprd.so", 2);
	if (!d.library) { printf("dlopen: %s\n", dlerror()); return 1; }
	printf("FACTORY_AVC_LIBRARY_LOADED\n");
	fflush(NULL);
	d.init = symbol(d.library, "H264DecInit");
	d.decode = symbol(d.library, "H264DecDecode");
	d.mem_init = symbol(d.library, "H264DecMemInit");
	d.set_picture = symbol(d.library, "H264Dec_SetCurRecPic");
	d.release = symbol(d.library, "H264DecRelease");
	d.last = symbol(d.library, "H264Dec_GetLastDspFrm");
	d.get_info = symbol(d.library, "H264DecGetInfo");
	d.h.cookie = &d;
	d.h.bind = d.h.unbind = binding;
	d.h.extra = extra;
	d.h.mbinfo = mbinfo;
	memory[0] = (uint64_t)(uintptr_t)internal;
	memory[2] = 2 * 1024 * 1024;
	ret = d.init(&d.h, memory, format, config);
	printf("H264DecInit result=%d state=%p\n", ret, d.h.state);
	check(!ret && d.h.state, "factory decoder initialization");
	/* The factory's context fd is at byte 8, confirmed by its OPEN/Get_IOVA code. */
	d.fd = (int)get32(d.h.state, 8);
	check(d.fd > 0, "decoder owns device fd");
	if (init_only) goto release;
	d.output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(d.output >= 0, "create unique output file");
	for (unsigned int i = 0; i < FRAMES; i++) frames[i] = allocate(&d, FRAME_SIZE);
	stream = allocate(&d, 1024 * 1024);
	int input = open(argv[1], O_CLOEXEC);
	check(input >= 0, "open generated H264 test stream");
	void *source = calloc(1, stream->size);
	check(source != NULL, "retained original bitstream");
	long length = read(input, source, stream->size - 4096);
	check(length > 0 && length < (long)stream->size - 4096, "read bounded H264 stream");
	close(input);
	long offset = 0;
	for (unsigned int call = 0; offset < length && call < 128; call++) {
		uint64_t in[32] = {0}, out[32] = {0}, info[16] = {0};
		struct buffer *frame = frames[call % FRAMES];
		d.set_picture(&d.h, frame->cpu, frame->map.iova_addr, frame, call, frame->fd);
		/* Native BSM consumes aligned buffers. Repack the remaining bytes at
		 * the aligned base instead of advancing the hardware buffer address.
		 */
		sync_buffer(stream, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
		memcpy(stream->cpu, (char *)source + offset, length - offset);
		memset((char *)stream->cpu + length - offset, 0, 4096);
		sync_buffer(stream, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		put64(in, 0, (uint64_t)(uintptr_t)stream->cpu);
		put64(in, 8, stream->map.iova_addr);
		put32(in, 16, length - offset);
		put32(in, 20, 1); /* beLastFrm */
		put64(in, 32, call); /* presentation timestamp */
		ret = d.decode(&d.h, in, out);
		uint32_t consumed = get32(in, 16);
		printf("DECODE call=%u offset=%ld result=%d consumed=%u effective=%u "
		       "width=%u height=%u pic=%u info=%d\n", call, offset, ret, consumed,
		       get32(out, 48), get32(out, 24), get32(out, 28), get32(out, 68),
		       d.get_info(&d.h, info));
		fflush(NULL);
		if (get32(out, 48)) {
			struct buffer *b = (void *)(uintptr_t)out[7];
			check(b >= d.buffers && b < d.buffers + d.count, "real output header in retained pool");
			d.width = get32(out, 24); d.height = get32(out, 28);
			save_frame(&d, b);
		}
		check(ret == 0 || ret == -9, "factory decoder accepted stream");
		check(consumed && consumed <= (unsigned long)(length - offset), "decoder consumed bounded input");
		offset += consumed;
	}
	check(offset == length, "complete bitstream consumed");
	for (unsigned int i = 0; i < FRAMES; i++) {
		void *header = NULL;
		unsigned int index = 0;
		uint64_t pts = 0;
		ret = d.last(&d.h, &header, &index, &pts);
		printf("DRAIN result=%d header=%p index=%u pts=%lu\n", ret, header, index, (unsigned long)pts);
		if (ret || !header) break;
		struct buffer *b = header;
		check(b >= d.buffers && b < d.buffers + d.count, "drained output belongs to retained pool");
		save_frame(&d, b);
	}
	check(d.outputs > 0, "hardware produced a decoded frame");
	free(source);
	close(d.output);
release:
	ret = d.release(&d.h);
	printf("H264DecRelease result=%d outputs=%u\n", ret, d.outputs);
	check(!ret, "factory decoder released and kernel DMA quiesced");
	for (unsigned int i = 0; i < d.count; i++) {
		munmap(d.buffers[i].cpu, d.buffers[i].size);
		close(d.buffers[i].fd);
	}
	free(internal);
	dlclose(d.library);
	printf("%s\n", init_only ? "FACTORY_AVC_INIT_RELEASE_PASS" : "FACTORY_AVC_HARDWARE_OUTPUT_READY_FOR_REFERENCE_COMPARISON");
	return 0;
}

/* Match Android 13's dynamic crtbegin ABI, including Bionic TLS initialization. */
__attribute__((used, noreturn)) static void start_main(void *raw)
{
	const void *structors[3] = {NULL, NULL, NULL};
	__libc_init(raw, NULL, main, structors);
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\n"
	"bti j\nmov x29,xzr\nmov x30,xzr\nmov x0,sp\nb start_main\n"
	".size _start, .-_start\n");
