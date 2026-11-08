// SPDX-License-Identifier: GPL-2.0-only
/* Standard Media Request acceptance client. No private codec ABI or MMIO.
 * Uses two alternating CAPTURE buffers, retaining only the preceding picture.
 * The fixture parser accepts generated single-reference Baseline IP streams.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <drm/panfrost_drm.h>
#ifdef W202_REQUEST_IPB
#include "h264-ipb-fixture.h"
typedef struct h264_ipb_fixture request_fixture;
#define REQUEST_NAME "IPB"
#define REQUEST_MAX_FRAMES W202_IPB_FIXTURE_FRAMES
#define parse_fixture h264_parse_ipb
#else
#include "h264-ip-fixture.h"
typedef struct h264_ip_fixture request_fixture;
#define REQUEST_NAME "IP"
#define REQUEST_MAX_FRAMES 2
#define parse_fixture h264_parse_ip
#endif

static int decoder = -1;
static void check(bool ok, const char *message)
{
	if (ok)
		return;
	fprintf(stderr, "Request " REQUEST_NAME " error: %s (errno=%d: %s)\n", message, errno, strerror(errno));
	if (decoder >= 0)
		close(decoder);
	exit(1);
}

struct request_buffer {
	enum v4l2_buf_type type;
	enum v4l2_memory memory;
	unsigned int index;
	void *data;
	size_t length;
	int fd;
};

static void cpu_sync(struct request_buffer *buffer, uint64_t flags)
{
	if (buffer->fd >= 0) {
		struct dma_buf_sync sync = {.flags = flags};

		check(!ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync), "DMA-BUF CPU sync");
	}
}

static void allocate_queue(enum v4l2_buf_type type, bool dmabuf, unsigned int count)
{
	struct v4l2_requestbuffers request = {
		.type = type, .memory = dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
		.count = count,
	};

	check(!ioctl(decoder, VIDIOC_REQBUFS, &request) && request.count >= count, "REQBUFS");
}

static struct request_buffer map_buffer(enum v4l2_buf_type type, bool dmabuf,
					unsigned int index, size_t size)
{
	struct request_buffer buffer = {
		.type = type, .memory = dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
		.index = index, .fd = -1,
	};

	if (dmabuf) {
		int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		struct drm_panfrost_create_bo bo = {.size = (size + 4095) & ~(size_t)4095};
		struct drm_prime_handle prime;

		check(render >= 0, "Panfrost render device");
		check(!ioctl(render, DRM_IOCTL_PANFROST_CREATE_BO, &bo), "Panfrost BO");
		prime = (struct drm_prime_handle){.handle = bo.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
		check(!ioctl(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "export DMA-BUF");
		close(render);
		buffer.fd = prime.fd;
		buffer.length = bo.size;
		buffer.data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, buffer.fd, 0);
	} else {
		struct v4l2_buffer info = {.type = type, .memory = buffer.memory, .index = index};

		check(!ioctl(decoder, VIDIOC_QUERYBUF, &info), "QUERYBUF");
		buffer.length = info.length;
		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE && getenv("W202_EARLY_EXPORT")) {
			struct v4l2_exportbuffer exported = {
				.type = type, .index = index, .flags = O_CLOEXEC | O_RDWR,
			};

			check(!ioctl(decoder, VIDIOC_EXPBUF, &exported), "export CAPTURE before decoding");
			buffer.fd = exported.fd;
			buffer.data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					   MAP_SHARED, buffer.fd, 0);
		} else {
			buffer.data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					   MAP_SHARED, decoder, info.m.offset);
		}
	}
	check(buffer.data != MAP_FAILED && buffer.length >= size, "buffer mapping and capacity");
	return buffer;
}

static void enqueue(struct request_buffer *buffer, size_t bytes, int request_fd,
		    unsigned int timestamp_seconds)
{
	struct v4l2_buffer entry = {
		.type = buffer->type, .memory = buffer->memory, .index = buffer->index,
		.bytesused = bytes, .length = buffer->length, .field = V4L2_FIELD_NONE,
		.timestamp = {.tv_sec = timestamp_seconds},
	};

	if (buffer->memory == V4L2_MEMORY_DMABUF)
		entry.m.fd = buffer->fd;
	if (request_fd >= 0) {
		entry.flags = V4L2_BUF_FLAG_REQUEST_FD;
		entry.request_fd = request_fd;
	}
	check(!ioctl(decoder, VIDIOC_QBUF, &entry), "QBUF");
}

static struct v4l2_buffer dequeue(struct request_buffer *buffer)
{
	struct v4l2_buffer entry = {.type = buffer->type, .memory = buffer->memory};

	check(!ioctl(decoder, VIDIOC_DQBUF, &entry) && entry.index == buffer->index, "DQBUF/index");
	return entry;
}

static void write_bytes(int fd, const void *data, size_t bytes)
{
	const char *p = data;

	while (bytes) {
		ssize_t n = write(fd, p, bytes);

		if (n < 0 && errno == EINTR)
			continue;
		check(n > 0, "pixel output");
		p += n;
		bytes -= n;
	}
}

static void dump_picture(int fd, struct request_buffer *buffer,
			 const struct h264_fixture *fixture, unsigned int stride,
			 unsigned int coded_height)
{
	cpu_sync(buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	const unsigned char *y = (unsigned char *)buffer->data +
		fixture->crop_top * stride + fixture->crop_left;
	const unsigned char *uv = (unsigned char *)buffer->data + stride * coded_height +
		(fixture->crop_top / 2) * stride + fixture->crop_left;

	for (unsigned int row = 0; row < fixture->height; row++)
		write_bytes(fd, y + row * stride, fixture->width);
	for (unsigned int row = 0; row < fixture->height / 2; row++)
		write_bytes(fd, uv + row * stride, fixture->width);
	cpu_sync(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

#ifdef W202_REQUEST_IPB
static unsigned char *save_picture(struct request_buffer *buffer,
				   const struct h264_fixture *fixture,
				   unsigned int stride, unsigned int coded_height)
{
	size_t pixels = (size_t)fixture->width * fixture->height;
	unsigned char *picture = malloc(pixels * 3 / 2);

	check(picture != NULL, "save reused CAPTURE pixels");
	cpu_sync(buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	const unsigned char *y = (unsigned char *)buffer->data + fixture->crop_top * stride + fixture->crop_left;
	const unsigned char *uv = (unsigned char *)buffer->data + stride * coded_height +
		(fixture->crop_top / 2) * stride + fixture->crop_left;
	for (unsigned int row = 0; row < fixture->height; row++)
		memcpy(picture + row * fixture->width, y + row * stride, fixture->width);
	for (unsigned int row = 0; row < fixture->height / 2; row++)
		memcpy(picture + pixels + row * fixture->width, uv + row * stride, fixture->width);
	cpu_sync(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	return picture;
}
#endif

static void stream(bool on, struct request_buffer *input, struct request_buffer *frame)
{
	unsigned long command = on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF;

	check(!ioctl(decoder, command, &frame->type), "CAPTURE stream state");
	check(!ioctl(decoder, command, &input->type), "OUTPUT stream state");
}

static void replace_dmabuf(struct request_buffer *buffer, int fd, size_t length)
{
	check(fd >= 0 && length >= buffer->length, "alias DMA-BUF capacity");
	check(!munmap(buffer->data, buffer->length) && !close(buffer->fd), "replace original DMA-BUF");
	buffer->fd = fd;
	buffer->length = length;
	buffer->data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(buffer->data != MAP_FAILED, "map aliased DMA-BUF");
}

static void unmap_buffer(struct request_buffer *buffer)
{
	if (buffer->data) {
		check(!munmap(buffer->data, buffer->length), "unmap buffer");
		buffer->data = NULL;
	}
}

static void alias_exports(const char *device, unsigned int width, unsigned int height,
			  struct request_buffer *frames)
{
	int allocator = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	struct v4l2_requestbuffers request = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .count = 1,
	};
	struct v4l2_buffer info = {.type = request.type, .memory = request.memory};

	check(allocator >= 0, "independent MMAP allocation context");
	format.fmt.pix = (struct v4l2_pix_format){
		.width = width, .height = height, .pixelformat = V4L2_PIX_FMT_H264_SLICE,
	};
	check(!ioctl(allocator, VIDIOC_S_FMT, &format) &&
	      !ioctl(allocator, VIDIOC_REQBUFS, &request) && request.count &&
	      !ioctl(allocator, VIDIOC_QUERYBUF, &info), "allocate alias source");
	for (unsigned int i = 0; i < 2; i++) {
		struct v4l2_exportbuffer exported = {.type = request.type, .flags = O_CLOEXEC | O_RDWR};

		check(!ioctl(allocator, VIDIOC_EXPBUF, &exported), "export same allocation twice");
		replace_dmabuf(&frames[i], exported.fd, info.length);
	}
	/* Each export owns the allocation after the original vb2 context closes. */
	check(!close(allocator), "close independent allocation context");
}

int main(int argc, char **argv)
{
	check(argc == 6,
	      "usage: w202-vpu-request-ip[b] INPUT.h264 OUTPUT.nv12 VIDEO MEDIA mmap|dmabuf");
	check(!strcmp(argv[5], "mmap") || !strcmp(argv[5], "dmabuf"), "memory mode");
	bool dmabuf = !strcmp(argv[5], "dmabuf");
	const char *bad_ref = getenv("W202_BAD_REF");
	const char *bad_l1 = getenv("W202_BAD_L1");
	const char *alias = getenv("W202_DMA_ALIAS");
	bool cancel_queued = !!getenv("W202_CANCEL_QUEUED");
	bool reuse = !!getenv("W202_CAPTURE_REUSE");
	bool restart = !!getenv("W202_RESTART"), missing = !!getenv("W202_MISSING_CONTROL");
	bool flat_matrix = !!getenv("W202_FLAT_MATRIX");
	struct v4l2_ctrl_h264_scaling_matrix scaling;

	memset(&scaling, 16, sizeof(scaling));
	check(!bad_ref || !strcmp(bad_ref, "missing") || !strcmp(bad_ref, "invalid"), "bad-ref mode");
	check(!alias || (dmabuf && (!strcmp(alias, "input") || !strcmp(alias, "reference") ||
	      !strcmp(alias, "reference-export"))), "DMA alias mode requires imported buffers");
#ifdef W202_REQUEST_IPB
	check(!bad_l1 || !strcmp(bad_l1, "missing") || !strcmp(bad_l1, "invalid") ||
	      !strcmp(bad_l1, "self"), "bad-L1 mode");
#else
	check(!bad_l1, "bad-L1 mode needs an IPB fixture");
	check(!reuse, "three-buffer reference reuse needs an IPB fixture");
#endif
	FILE *source = fopen(argv[1], "rb");
	check(source && !fseek(source, 0, SEEK_END), "open input");
	long bytes = ftell(source);
	check(bytes > 0 && bytes <= 8 * 1024 * 1024 && !fseek(source, 0, SEEK_SET), "input size");
	unsigned char *data = malloc(bytes);
	check(data && fread(data, 1, bytes, source) == (size_t)bytes, "read input");
	fclose(source);
	request_fixture *fixture = malloc(sizeof(*fixture));
	check(fixture && !parse_fixture(data, bytes, fixture), "parse " REQUEST_NAME " fixture");
	const char *poc_origin = getenv("W202_POC_OFFSET");
	if (poc_origin) {
		check(!strcmp(poc_origin, "65536") || !strcmp(poc_origin, "-65536"), "POC offset fixture");
		int offset = !strcmp(poc_origin, "65536") ? 65536 : -65536;

		for (unsigned int i = 0; i < fixture->count; i++) {
			struct v4l2_ctrl_h264_decode_params *d = &fixture->frame[i].decode;

			d->top_field_order_cnt += offset;
			d->bottom_field_order_cnt += offset;
			for (unsigned int j = 0; j < V4L2_H264_NUM_DPB_ENTRIES; j++) {
				if (!(d->dpb[j].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
					continue;
				d->dpb[j].top_field_order_cnt += offset;
				d->dpb[j].bottom_field_order_cnt += offset;
			}
		}
	}
	check(fixture->count > 1 || (!bad_ref && !restart), "reference test needs P pictures");
	check(!alias || !strcmp(alias, "input") || fixture->count > 1,
	      "reference alias test needs P pictures");
	const struct h264_fixture *first = &fixture->frame[0];
	unsigned int width = (first->sps.pic_width_in_mbs_minus1 + 1) * 16;
	unsigned int height = (first->sps.pic_height_in_map_units_minus1 + 1) * 16;
	size_t max_nal = 0;
	for (unsigned int i = 0; i < fixture->count; i++)
		if (max_nal < fixture->frame[i].nal_size)
			max_nal = fixture->frame[i].nal_size;
	decoder = open(argv[3], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	check(decoder >= 0, "V4L2 device");
	int media = open(argv[4], O_RDWR | O_CLOEXEC), request_fd = -1;
	check(media >= 0 && !ioctl(media, MEDIA_IOC_REQUEST_ALLOC, &request_fd), "allocate media request");
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	format.fmt.pix = (struct v4l2_pix_format){
		.width = width, .height = height, .pixelformat = V4L2_PIX_FMT_H264_SLICE,
		.sizeimage = max_nal + 1024,
	};
	check(!ioctl(decoder, VIDIOC_S_FMT, &format), "OUTPUT format");
	allocate_queue(format.type, dmabuf, 1);
	struct request_buffer input = map_buffer(format.type, dmabuf, 0, format.fmt.pix.sizeimage);
	format = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
	check(!ioctl(decoder, VIDIOC_G_FMT, &format) && format.fmt.pix.bytesperline == width &&
	      format.fmt.pix.height == height && format.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12,
	      "CAPTURE NV12 format");
	unsigned int frame_count = 2;
#ifdef W202_REQUEST_IPB
	frame_count = reuse ? 3 : fixture->count;
	unsigned int slots[W202_IPB_FIXTURE_FRAMES] = {};
	unsigned char *saved[W202_IPB_FIXTURE_FRAMES] = {};
#endif
	allocate_queue(format.type, dmabuf, frame_count);
	struct request_buffer frames[REQUEST_MAX_FRAMES];
	for (unsigned int i = 0; i < frame_count; i++)
		frames[i] = map_buffer(format.type, dmabuf, i, format.fmt.pix.sizeimage);
	if (alias) {
		if (!strcmp(alias, "input"))
			replace_dmabuf(&frames[0], fcntl(input.fd, F_DUPFD_CLOEXEC, 0), input.length);
		else if (!strcmp(alias, "reference"))
			replace_dmabuf(&frames[1], fcntl(frames[0].fd, F_DUPFD_CLOEXEC, 0), frames[0].length);
		else
			alias_exports(argv[3], width, height, frames);
	}
	int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(output >= 0, "new output file");
	bool streaming = false;
	unsigned int completed = 0, errors = 0, sequence = 0;
	for (unsigned int i = 0; i < fixture->count; i++) {
		struct h264_fixture *f = &fixture->frame[i];
		struct request_buffer *frame = &frames[i % 2];
		bool expected_error = i == 1 && (bad_ref || restart);

		if (alias && i == (unsigned int)!!strcmp(alias, "input"))
			expected_error = true;
		if (flat_matrix)
			f->pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
		if (i)
			f->decode.dpb[0].reference_ts = (uint64_t)i * 1000000000;
#ifdef W202_REQUEST_IPB
		unsigned int slot = i;
		if (reuse) {
			unsigned int l0 = i ? slots[fixture->ref0[i]] : frame_count;
			unsigned int l1 = f->slice.slice_type == V4L2_H264_SLICE_TYPE_B ?
					 slots[fixture->ref1[i]] : frame_count;

			for (slot = 0; slot < frame_count; slot++)
				if (slot != l0 && slot != l1)
					break;
			check(slot < frame_count, "free CAPTURE excluding active L0/L1");
		}
		slots[i] = slot;
		frame = &frames[slot];
		if (i)
			f->decode.dpb[0].reference_ts = (uint64_t)(fixture->ref0[i] + 1) * 1000000000;
		if (f->slice.slice_type == V4L2_H264_SLICE_TYPE_B) {
			f->decode.dpb[1].reference_ts = (uint64_t)(fixture->ref1[i] + 1) * 1000000000;
			if (bad_l1) {
				expected_error = true;
				if (!strcmp(bad_l1, "invalid"))
					f->decode.dpb[1].flags = 0;
				else
					f->decode.dpb[1].reference_ts = !strcmp(bad_l1, "self") ?
						(uint64_t)(i + 1) * 1000000000 : UINT64_C(999000000000);
			}
		}
#endif
		if (i == 1 && bad_ref) {
			if (!strcmp(bad_ref, "missing"))
				f->decode.dpb[0].reference_ts = UINT64_C(999000000000);
			else
				f->decode.dpb[0].flags = 0;
		}
		cpu_sync(&input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
		memset(input.data, 0, input.length);
		memcpy(input.data, f->nal, f->nal_size);
		cpu_sync(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		struct v4l2_ext_control controls[] = {
			{.id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(f->sps), .ptr = &f->sps},
			{.id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(f->pps), .ptr = &f->pps},
			{.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof(f->slice), .ptr = &f->slice},
			{.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof(f->decode), .ptr = &f->decode},
			{.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX, .size = sizeof(f->scaling),
			 .ptr = flat_matrix ? &scaling : &f->scaling},
		};
		struct v4l2_ext_controls parameters = {
			.which = V4L2_CTRL_WHICH_REQUEST_VAL,
			.count = missing ? 3 : (f->pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) ? 5 : 4,
			.request_fd = request_fd, .controls = controls,
		};
		check(!ioctl(decoder, VIDIOC_S_EXT_CTRLS, &parameters), "request controls");
		enqueue(frame, 0, -1, 0);
		enqueue(&input, f->nal_size, request_fd, i + 1);
		if (missing) {
			check(ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE) == -1 && errno == ENOENT,
			      "missing control rejection");
			fprintf(stderr, "V4L2 Request " REQUEST_NAME ": missing required decode control rejected before streaming\n");
			break;
		}
		check(!ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE), "queue media request");
		if (!i && cancel_queued) {
			struct pollfd canceled = {.fd = request_fd, .events = POLLPRI};

			/* MMAP VMAs retain the video file after close(fd). Drop those
			 * references so this actually exercises the driver's release. */
			unmap_buffer(&input);
			for (unsigned int at = 0; at < frame_count; at++)
				unmap_buffer(&frames[at]);
			check(!close(decoder), "close queued request before STREAMON");
			decoder = -1;
			check(poll(&canceled, 1, 3000) == 1 && (canceled.revents & POLLPRI),
			      "queued request completion after close");
			check(!ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT), "reinitialize canceled request");
			fprintf(stderr, "V4L2 Request " REQUEST_NAME ": queued request canceled before STREAMON\n");
			break;
		}
		if (!streaming) {
			stream(true, &input, frame);
			streaming = true;
		}
		struct pollfd done = {.fd = request_fd, .events = POLLPRI};
		int polled;
		do {
			polled = poll(&done, 1, 3000);
		} while (polled < 0 && errno == EINTR);
		check(polled == 1 && (done.revents & POLLPRI), "request completion poll");
		struct v4l2_buffer captured = dequeue(frame), consumed = dequeue(&input);
		bool failed = (captured.flags | consumed.flags) & V4L2_BUF_FLAG_ERROR;
		check(failed == expected_error, "buffer completion status");
		check(captured.bytesused == width * height * 3 / 2 &&
		      captured.sequence == sequence && consumed.sequence == sequence &&
		      captured.timestamp.tv_sec == i + 1 && !captured.timestamp.tv_usec,
		      "capture payload, sequence and timestamp");
		sequence++;
		completed++;
		errors += failed;
		fprintf(stderr, "V4L2 Request " REQUEST_NAME " frame=%u type=%u buffer=%u ref_ns=%llu error=%u\n",
			i, f->slice.slice_type, frame->index,
			(unsigned long long)f->decode.dpb[0].reference_ts, failed);
#ifndef W202_REQUEST_IPB
		if (!failed)
			dump_picture(output, frame, f, width, height);
#else
		if (!failed && reuse)
			saved[i] = save_picture(frame, f, width, height);
#endif
		if (expected_error)
			break;
		if (i + 1 != fixture->count)
			check(!ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT), "reinitialize request");
		if (!i && restart) {
			stream(false, &input, frame);
			streaming = false;
			sequence = 0;
		}
	}
#ifdef W202_REQUEST_IPB
	check(!bad_l1 || errors, "negative L1 test reached a B picture");
	if (!errors && completed == fixture->count)
		for (unsigned int display = 0; display < fixture->count; display++) {
			unsigned int index = fixture->display_order[display];

			if (reuse) {
				check(saved[index] != NULL, "display frame retained before buffer reuse");
				write_bytes(output, saved[index], (size_t)first->width * first->height * 3 / 2);
			} else {
				dump_picture(output, &frames[index], &fixture->frame[index], width, height);
			}
		}
#endif
	if (streaming)
		stream(false, &input, &frames[0]);
	check(!close(output), "close pixel output");
	fprintf(stderr, "V4L2 Request " REQUEST_NAME ": coded=%ux%u visible=%ux%u mode=%s requests=%u errors=%u restart=%u missing=%u\n",
		width, height, first->width, first->height, dmabuf ? "DMABUF" : "MMAP",
		completed, errors, restart, missing);
	close(request_fd);
	close(media);
	if (decoder >= 0)
		close(decoder);
	decoder = -1;
	unmap_buffer(&input);
	if (input.fd >= 0)
		close(input.fd);
	for (unsigned int i = 0; i < frame_count; i++) {
		unmap_buffer(&frames[i]);
		if (frames[i].fd >= 0)
			close(frames[i].fd);
	}
#ifdef W202_REQUEST_IPB
	for (unsigned int i = 0; i < fixture->count; i++)
		free(saved[i]);
#endif
	free(fixture);
	free(data);
	return 0;
}
