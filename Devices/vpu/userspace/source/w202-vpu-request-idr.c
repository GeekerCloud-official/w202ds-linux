// SPDX-License-Identifier: GPL-2.0-only
/* Real Media Request API acceptance client; no private codec ioctl/MMIO. */
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
#include "h264-idr-fixture.h"

static int decoder = -1;
static void check(bool ok, const char *message)
{
	if (ok)
		return;
	fprintf(stderr, "Request IDR error: %s (errno=%d: %s)\n", message, errno, strerror(errno));
	if (decoder >= 0)
		close(decoder);
	exit(1);
}

struct request_buffer {
	enum v4l2_buf_type type;
	enum v4l2_memory memory;
	void *data;
	size_t length;
	int fd;
};

static void cpu_sync(struct request_buffer *buffer, uint64_t flags)
{
	if (buffer->memory == V4L2_MEMORY_DMABUF) {
		struct dma_buf_sync sync = {.flags = flags};

		check(!ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync), "DMA-BUF CPU sync");
	}
}

static struct request_buffer allocate(enum v4l2_buf_type type, bool dmabuf, size_t size)
{
	struct request_buffer buffer = {
		.type = type, .memory = dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, .fd = -1,
	};
	struct v4l2_requestbuffers request = {.type = type, .memory = buffer.memory, .count = 1};

	check(!ioctl(decoder, VIDIOC_REQBUFS, &request) && request.count, "REQBUFS");
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
		struct v4l2_buffer info = {.type = type, .memory = buffer.memory};

		check(!ioctl(decoder, VIDIOC_QUERYBUF, &info), "QUERYBUF");
		buffer.length = info.length;
		buffer.data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, decoder, info.m.offset);
	}
	check(buffer.data != MAP_FAILED && buffer.length >= size, "buffer mapping and capacity");
	return buffer;
}

static void enqueue(struct request_buffer *buffer, size_t bytes, int request_fd)
{
	struct v4l2_buffer entry = {
		.type = buffer->type, .memory = buffer->memory, .bytesused = bytes,
		.length = buffer->length, .field = V4L2_FIELD_NONE,
		.timestamp = {.tv_sec = 1},
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

	check(!ioctl(decoder, VIDIOC_DQBUF, &entry), "DQBUF");
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

int main(int argc, char **argv)
{
	check(argc >= 5 && argc <= 7,
	      "usage: w202-vpu-request-idr INPUT.h264 OUTPUT.nv12 VIDEO MEDIA [mmap|dmabuf] [REPEAT]");
	bool dmabuf = argc >= 6 && !strcmp(argv[5], "dmabuf");
	unsigned int repeat = argc == 7 ? strtoul(argv[6], NULL, 10) : 1;
	check(repeat && repeat <= 10000, "repeat range");
	FILE *source = fopen(argv[1], "rb");
	check(source && !fseek(source, 0, SEEK_END), "open input");
	long bytes = ftell(source);
	check(bytes > 0 && bytes <= 8 * 1024 * 1024 && !fseek(source, 0, SEEK_SET), "input size");
	unsigned char *stream = malloc(bytes);
	check(stream && fread(stream, 1, bytes, source) == (size_t)bytes, "read input");
	fclose(source);
	struct h264_fixture fixture;
	check(!h264_parse_idr(stream, bytes, &fixture), "parse fixture");
	const char *bad_matrix = getenv("W202_BAD_MATRIX");
	bool matrix_error = bad_matrix && strcmp(bad_matrix, "unused8");

	check(!bad_matrix || !strcmp(bad_matrix, "missing") || !strcmp(bad_matrix, "zero4") ||
	      !strcmp(bad_matrix, "zero8") || !strcmp(bad_matrix, "unused8"), "matrix test mode");
	check(!bad_matrix || (fixture.pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT),
	      "matrix test requires explicit scaling lists");
	if (bad_matrix && !strcmp(bad_matrix, "zero4"))
		fixture.scaling.scaling_list_4x4[5][15] = 0;
	if (bad_matrix && !strcmp(bad_matrix, "zero8")) {
		check(fixture.pps.flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE, "8x8 matrix test");
		fixture.scaling.scaling_list_8x8[1][63] = 0;
	}
	if (bad_matrix && !strcmp(bad_matrix, "unused8"))
		memset(&fixture.scaling.scaling_list_8x8[2], 0, 4 * 64);
	if (getenv("W202_INVALID_PROFILE"))
		fixture.sps.profile_idc = 110;
	unsigned int width = (fixture.sps.pic_width_in_mbs_minus1 + 1) * 16;
	unsigned int height = (fixture.sps.pic_height_in_map_units_minus1 + 1) * 16;
	decoder = open(argv[3], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	check(decoder >= 0, "V4L2 device");
	int media = open(argv[4], O_RDWR | O_CLOEXEC), request_fd = -1;
	check(media >= 0 && !ioctl(media, MEDIA_IOC_REQUEST_ALLOC, &request_fd), "allocate media request");
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	format.fmt.pix = (struct v4l2_pix_format){
		.width = width, .height = height, .pixelformat = V4L2_PIX_FMT_H264_SLICE,
		.sizeimage = fixture.nal_size + 1024,
	};
	check(!ioctl(decoder, VIDIOC_S_FMT, &format), "OUTPUT format");
	struct request_buffer input = allocate(format.type, dmabuf, format.fmt.pix.sizeimage);
	format = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
	check(!ioctl(decoder, VIDIOC_G_FMT, &format) && format.fmt.pix.bytesperline == width &&
	      format.fmt.pix.height == height && format.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12,
	      "CAPTURE NV12 format");
	struct request_buffer frame = allocate(format.type, dmabuf, format.fmt.pix.sizeimage);
	cpu_sync(&input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	memset(input.data, 0, input.length);
	memcpy(input.data, fixture.nal, fixture.nal_size);
	cpu_sync(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	struct v4l2_ext_control controls[] = {
		{.id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(fixture.sps), .ptr = &fixture.sps},
		{.id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(fixture.pps), .ptr = &fixture.pps},
		{.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof(fixture.slice), .ptr = &fixture.slice},
		{.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof(fixture.decode), .ptr = &fixture.decode},
		{.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX, .size = sizeof(fixture.scaling), .ptr = &fixture.scaling},
	};
	struct v4l2_ext_controls parameters = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.count = (fixture.pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) &&
			 !(bad_matrix && !strcmp(bad_matrix, "missing")) ? 5 : 4,
		.request_fd = request_fd, .controls = controls,
	};
	int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(output >= 0, "new output file");
	for (unsigned int i = 0; i < repeat; i++) {
		check(!ioctl(decoder, VIDIOC_S_EXT_CTRLS, &parameters), "request controls");
		enqueue(&frame, 0, -1);
		enqueue(&input, fixture.nal_size, request_fd);
		check(!ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE), "queue media request");
		if (!i) {
			check(!ioctl(decoder, VIDIOC_STREAMON, &frame.type), "CAPTURE STREAMON");
			check(!ioctl(decoder, VIDIOC_STREAMON, &input.type), "OUTPUT STREAMON");
		}
		struct pollfd done = {.fd = request_fd, .events = POLLPRI};
		check(poll(&done, 1, 3000) == 1 && (done.revents & POLLPRI), "request completion poll");
		struct v4l2_buffer captured = dequeue(&frame), consumed = dequeue(&input);
		bool failed = (captured.flags | consumed.flags) & V4L2_BUF_FLAG_ERROR;
		check(failed == (matrix_error || !!getenv("W202_INVALID_PROFILE")), "buffer completion status");
		check(captured.bytesused == width * height * 3 / 2 &&
		      captured.sequence == i && consumed.sequence == i && captured.timestamp.tv_sec == 1,
		      "capture payload, sequence and timestamp");
		if (!failed) {
			cpu_sync(&frame, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
			unsigned char *y = (unsigned char *)frame.data + fixture.crop_top * width + fixture.crop_left;
			unsigned char *uv = (unsigned char *)frame.data + width * height +
				(fixture.crop_top / 2) * width + fixture.crop_left;

			for (unsigned int row = 0; row < fixture.height; row++)
				write_bytes(output, y + row * width, fixture.width);
			for (unsigned int row = 0; row < fixture.height / 2; row++)
				write_bytes(output, uv + row * width, fixture.width);
			cpu_sync(&frame, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
		}
		if (i + 1 != repeat)
			check(!ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT), "reinitialize request");
	}
	check(!ioctl(decoder, VIDIOC_STREAMOFF, &input.type), "OUTPUT STREAMOFF");
	check(!ioctl(decoder, VIDIOC_STREAMOFF, &frame.type), "CAPTURE STREAMOFF");
	check(!close(output), "close pixel output");
	fprintf(stderr, "V4L2 Request IDR: coded=%ux%u visible=%ux%u mode=%s requests=%u bytes=%zu rbsp=%zu header=%u negative=%u\n",
		width, height, fixture.width, fixture.height, dmabuf ? "DMABUF" : "MMAP", repeat,
		fixture.nal_size, fixture.rbsp_size, fixture.slice.header_bit_size,
		matrix_error || !!getenv("W202_INVALID_PROFILE"));
	close(request_fd);
	close(media);
	close(decoder);
	decoder = -1;
	munmap(input.data, input.length);
	munmap(frame.data, frame.length);
	if (input.fd >= 0)
		close(input.fd);
	if (frame.fd >= 0)
		close(frame.fd);
	free(stream);
	return 0;
}
