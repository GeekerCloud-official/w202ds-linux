// SPDX-License-Identifier: GPL-2.0-only
/* Native stateless H.264 frontend. The first supported subset is deliberately
 * limited to independently validated progressive Baseline/Main single-slice
 * IDR/P and non-reference spatial-direct B pictures, one entry per active list.
 * vb2 owns CPU storage; the native job owns device DMA attachments until reset.
 */
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-vmalloc.h>
#include "w202_vpu_native.h"

#define W202_VIDEO_NAME "w202-vpu-h264"
#define W202_MAX_INPUT (8U * 1024 * 1024)

struct w202_vpu_video {
	struct device *dev;
	struct mutex lock;
	struct v4l2_device v4l2;
	struct media_device media;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
};

struct w202_video_context {
	struct v4l2_fh fh;
	struct w202_vpu_video *video;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *sps, *pps, *slice, *decode, *scaling;
	struct v4l2_pix_format output, capture;
	struct work_struct work;
	atomic_t abort;
	u32 sequence;
};

struct w202_video_buffer {
	struct v4l2_m2m_buffer m2m;
	bool decoded;
	bool reference;
};

static struct w202_video_buffer *buffer_state(struct vb2_buffer *buffer)
{
	return container_of(to_vb2_v4l2_buffer(buffer), struct w202_video_buffer, m2m.vb);
}

static void invalidate_references(struct w202_video_context *ctx)
{
	struct vb2_queue *queue = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned int i;

	for (i = 0; i < queue->max_num_buffers; i++) {
		struct vb2_buffer *buffer = vb2_get_buffer(queue, i);

		if (buffer)
			WRITE_ONCE(buffer_state(buffer)->decoded, false);
	}
}

static struct w202_video_context *file_context(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct w202_video_context, fh);
}

static u32 pixel_size(const struct v4l2_pix_format *format)
{
	return format->width * format->height * 3 / 2;
}

static u32 auxiliary_size(const struct v4l2_pix_format *format)
{
	return (format->width / 16) * (format->height / 16) * 80 + 1024;
}

static void format_fill(struct v4l2_pix_format *pix, bool output)
{
	pix->width = ALIGN(clamp(pix->width, 16U, 1920U), 16);
	pix->height = ALIGN(clamp(pix->height, 16U, 1088U), 16);
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	pix->flags = 0;
	pix->priv = 0;
	if (output) {
		pix->pixelformat = V4L2_PIX_FMT_H264_SLICE;
		pix->bytesperline = 0;
		pix->sizeimage = PAGE_ALIGN(clamp(pix->sizeimage,
						64U * 1024, W202_MAX_INPUT + W202_H264_BSM_PADDING));
	} else {
		pix->pixelformat = V4L2_PIX_FMT_NV12;
		pix->bytesperline = pix->width;
		/* Reference metadata travels with its picture, outside visible NV12. */
		pix->sizeimage = PAGE_ALIGN(pixel_size(pix) + auxiliary_size(pix));
	}
}

static struct v4l2_pix_format *queue_format(struct w202_video_context *ctx,
					 enum v4l2_buf_type type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->output : &ctx->capture;
}

static int video_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, W202_VIDEO_NAME, sizeof(cap->driver));
	strscpy(cap->card, "W202DS experimental H.264 IPB", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:w202-vpu", sizeof(cap->bus_info));
	return 0;
}

static int video_enum_format(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_TYPE_IS_OUTPUT(fmt->type) ?
		V4L2_PIX_FMT_H264_SLICE : V4L2_PIX_FMT_NV12;
	return 0;
}

static int video_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *size)
{
	if (size->index || (size->pixel_format != V4L2_PIX_FMT_H264_SLICE &&
			   size->pixel_format != V4L2_PIX_FMT_NV12))
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	size->stepwise = (struct v4l2_frmsize_stepwise){
		.min_width = 16, .max_width = 1920, .step_width = 16,
		.min_height = 16, .max_height = 1088, .step_height = 16,
	};
	return 0;
}

static int video_get_format(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct w202_video_context *ctx = file_context(file);

	fmt->fmt.pix = *queue_format(ctx, fmt->type);
	return 0;
}

static int video_try_format(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct w202_video_context *ctx = file_context(file);
	bool output = V4L2_TYPE_IS_OUTPUT(fmt->type);

	if (!output) {
		fmt->fmt.pix.width = ctx->output.width;
		fmt->fmt.pix.height = ctx->output.height;
	}
	format_fill(&fmt->fmt.pix, output);
	return 0;
}

static int video_set_format(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct w202_video_context *ctx = file_context(file);
	struct vb2_queue *queue = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, fmt->type);
	bool output = V4L2_TYPE_IS_OUTPUT(fmt->type);

	if (vb2_is_busy(queue) || (output && vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx))))
		return -EBUSY;
	video_try_format(file, priv, fmt);
	*queue_format(ctx, fmt->type) = fmt->fmt.pix;
	if (output) {
		ctx->capture.width = ctx->output.width;
		ctx->capture.height = ctx->output.height;
		format_fill(&ctx->capture, false);
	}
	return 0;
}

static int video_get_selection(struct file *file, void *priv,
			       struct v4l2_selection *selection)
{
	struct w202_video_context *ctx = file_context(file);

	if (selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    (selection->target != V4L2_SEL_TGT_COMPOSE &&
	     selection->target != V4L2_SEL_TGT_COMPOSE_DEFAULT &&
	     selection->target != V4L2_SEL_TGT_COMPOSE_BOUNDS))
		return -EINVAL;
	/* SPS crop is application state, absent from the standard SPS control. */
	selection->r = (struct v4l2_rect){.width = ctx->capture.width,
					.height = ctx->capture.height};
	return 0;
}

static int queue_setup(struct vb2_queue *queue, unsigned int *buffers,
		       unsigned int *planes, unsigned int sizes[],
		       struct device *alloc_devs[])
{
	struct w202_video_context *ctx = vb2_get_drv_priv(queue);
	u32 size = queue_format(ctx, queue->type)->sizeimage;

	if (*planes)
		return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = size;
	return 0;
}

static int buffer_prepare(struct vb2_buffer *buffer)
{
	struct w202_video_context *ctx = vb2_get_drv_priv(buffer->vb2_queue);
	bool output = V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type);
	u32 size = queue_format(ctx, buffer->vb2_queue->type)->sizeimage;
	u32 used = vb2_get_plane_payload(buffer, 0);

	if (vb2_plane_size(buffer, 0) < size || buffer->planes[0].data_offset)
		return -EINVAL;
	if (output) {
		if (!used || used > W202_MAX_INPUT ||
		    used + W202_H264_BSM_PADDING > vb2_plane_size(buffer, 0))
			return -EINVAL;
	} else {
		WRITE_ONCE(buffer_state(buffer)->decoded, false);
		vb2_set_plane_payload(buffer, 0, pixel_size(&ctx->capture));
	}
	to_vb2_v4l2_buffer(buffer)->field = V4L2_FIELD_NONE;
	return 0;
}

static void buffer_queue(struct vb2_buffer *buffer)
{
	struct w202_video_context *ctx = vb2_get_drv_priv(buffer->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(buffer));
}

static void buffer_request_complete(struct vb2_buffer *buffer)
{
	struct w202_video_context *ctx = vb2_get_drv_priv(buffer->vb2_queue);

	v4l2_ctrl_request_complete(buffer->req_obj.req, &ctx->controls);
}

static int buffer_out_validate(struct vb2_buffer *buffer)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(buffer);

	if (vbuf->field == V4L2_FIELD_ANY)
		vbuf->field = V4L2_FIELD_NONE;
	return vbuf->field == V4L2_FIELD_NONE ? 0 : -EINVAL;
}

static int start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct w202_video_context *ctx = vb2_get_drv_priv(queue);

	atomic_set(&ctx->abort, 0);
	ctx->sequence = 0;
	invalidate_references(ctx);
	return 0;
}

static void stop_streaming(struct vb2_queue *queue)
{
	struct w202_video_context *ctx = vb2_get_drv_priv(queue);
	struct vb2_v4l2_buffer *buffer;

	/* m2m has already canceled and waited for the executing job. */
	invalidate_references(ctx);
	for (;;) {
		buffer = V4L2_TYPE_IS_OUTPUT(queue->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buffer)
			break;
		if (buffer->vb2_buf.req_obj.req)
			buffer_request_complete(&buffer->vb2_buf);
		v4l2_m2m_buf_done(buffer, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops queue_ops = {
	.queue_setup = queue_setup,
	.buf_prepare = buffer_prepare,
	.buf_queue = buffer_queue,
	.buf_out_validate = buffer_out_validate,
	.buf_request_complete = buffer_request_complete,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *output, struct vb2_queue *capture)
{
	struct w202_video_context *ctx = priv;
	struct vb2_queue *queues[] = {output, capture};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(queues); i++) {
		struct vb2_queue *queue = queues[i];

		queue->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
		queue->io_modes = VB2_MMAP | VB2_DMABUF;
		queue->drv_priv = ctx;
		queue->buf_struct_size = sizeof(struct w202_video_buffer);
		queue->ops = &queue_ops;
		queue->mem_ops = &vb2_vmalloc_memops;
		queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		queue->lock = &ctx->video->lock;
		queue->dev = ctx->video->dev;
		queue->supports_requests = !i;
		queue->requires_requests = !i;
		ret = vb2_queue_init(queue);
		if (ret)
			return ret;
	}
	return 0;
}

static struct dma_buf *buffer_dmabuf(struct vb2_buffer *buffer)
{
	struct dma_buf *buf = buffer->planes[0].dbuf;

	if (buf) {
		get_dma_buf(buf);
		return buf;
	}
	/* A fresh export owns the allocation even after the vb2 queue is freed. */
	buf = buffer->vb2_queue->mem_ops->get_dmabuf(buffer,
					 buffer->planes[0].mem_priv, O_RDWR);
	return buf ? buf : ERR_PTR(-ENOMEM);
}

static int prepare_input(struct vb2_buffer *buffer, struct dma_buf *buf,
			 const struct w202_h264_job *job, u32 *rbsp)
{
	u32 i, zeros = 0, bytes = vb2_get_plane_payload(buffer, 0);
	u8 *data = vb2_plane_vaddr(buffer, 0);
	int ret, end;

	if (!data)
		return -EFAULT;
	ret = dma_buf_begin_cpu_access(buf, DMA_BIDIRECTIONAL);
	if (ret)
		return ret;
	/* START_CODE_NONE means the first byte is the NAL header. */
	if ((data[0] & 0x80) ||
	    (data[0] & 31) != (job->decode->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC ? 5 : 1) ||
	    ((data[0] >> 5) & 3) != job->decode->nal_ref_idc) {
		ret = -EINVAL;
		goto out;
	}
	*rbsp = bytes;
	for (i = 1; i < bytes; i++) {
		if (zeros == 2 && data[i] == 3) {
			if (i + 1 == bytes || data[i + 1] > 3) {
				ret = -EINVAL;
				goto out;
			}
			(*rbsp)--;
			zeros = 0;
		} else {
			zeros = data[i] ? 0 : min(zeros + 1, 2U);
		}
	}
out:
	end = dma_buf_end_cpu_access(buf, DMA_BIDIRECTIONAL);
	return ret ? ret : end;
}

static void decode_work(struct work_struct *work)
{
	struct w202_video_context *ctx = container_of(work, struct w202_video_context, work);
	struct w202_vpu_video *video = ctx->video;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	struct media_request *request = src->vb2_buf.req_obj.req;
	struct w202_h264_config *config = NULL;
	struct w202_h264_job job = {};
	struct codec_file *native = NULL;
	struct dma_buf *input = NULL, *frame = NULL, *reference[2] = {};
	struct w202_vpu_iova input_map = {}, frame_map = {}, reference_map[2] = {};
	u32 pixels = pixel_size(&ctx->capture), rbsp;
	unsigned int i, references;
	int ret, ended;

	ret = v4l2_ctrl_request_setup(request, &ctx->controls);
	if (ret)
		goto done;
	job.sps = ctx->sps->p_cur.p_h264_sps;
	job.pps = ctx->pps->p_cur.p_h264_pps;
	job.slice = ctx->slice->p_cur.p_h264_slice_params;
	job.decode = ctx->decode->p_cur.p_h264_decode_params;
	if (job.pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		struct v4l2_ctrl_handler *handler =
			v4l2_ctrl_request_hdl_find(request, &ctx->controls);
		bool present = handler && v4l2_ctrl_request_hdl_ctrl_find(handler,
						V4L2_CID_STATELESS_H264_SCALING_MATRIX);

		v4l2_ctrl_request_hdl_put(handler);
		if (!present) {
			ret = -ENOENT;
			goto done;
		}
		job.scaling = ctx->scaling->p_cur.p_h264_scaling_matrix;
	}
	if ((job.sps->pic_width_in_mbs_minus1 + 1) * 16 != ctx->capture.width ||
	    (job.sps->pic_height_in_map_units_minus1 + 1) * 16 != ctx->capture.height ||
	    job.slice->header_bit_size > 4096) {
		ret = -EINVAL;
		goto done;
	}
	input = buffer_dmabuf(&src->vb2_buf);
	if (IS_ERR(input)) {
		ret = PTR_ERR(input);
		input = NULL;
		goto done;
	}
	frame = buffer_dmabuf(&dst->vb2_buf);
	if (IS_ERR(frame)) {
		ret = PTR_ERR(frame);
		frame = NULL;
		goto done;
	}
	if (input == frame) {
		ret = -EINVAL;
		goto done;
	}
	ret = prepare_input(&src->vb2_buf, input, &job, &rbsp);
	if (ret)
		goto done;
	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config) {
		ret = -ENOMEM;
		goto done;
	}
	job.input_bytes = vb2_get_plane_payload(&src->vb2_buf, 0);
	job.nalu_bytes = rbsp;
	job.input_address = 0x40000000;
	job.output_address = 0x41000000;
	job.auxiliary_address = job.output_address + pixels;
	job.input_capacity = vb2_plane_size(&src->vb2_buf, 0);
	job.output_capacity = pixels;
	job.auxiliary_capacity = auxiliary_size(&ctx->capture);
	references = job.slice->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 :
		     job.slice->slice_type == V4L2_H264_SLICE_TYPE_P ? 1 : 0;
	for (i = 0; i < references; i++) {
		struct vb2_queue *queue = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
		struct vb2_buffer *buffer;
		unsigned int index = i ? job.slice->ref_pic_list1[0].index :
					 job.slice->ref_pic_list0[0].index;

		if (index >= V4L2_H264_NUM_DPB_ENTRIES) {
			ret = -EINVAL;
			goto done;
		}
		buffer = vb2_find_buffer(queue, job.decode->dpb[index].reference_ts);
		if (!buffer || buffer == &dst->vb2_buf || buffer->index > 31 ||
		    !READ_ONCE(buffer_state(buffer)->decoded) ||
		    !READ_ONCE(buffer_state(buffer)->reference)) {
			ret = -EINVAL;
			goto done;
		}
		reference[i] = buffer_dmabuf(buffer);
		if (IS_ERR(reference[i])) {
			ret = PTR_ERR(reference[i]);
			reference[i] = NULL;
			goto done;
		}
		if (reference[i] == frame || reference[i] == input) {
			ret = -EINVAL;
			goto done;
		}
		if (i) {
			job.reference1_address = 0x44000000;
			job.reference1_auxiliary_address = job.reference1_address + pixels;
			job.reference1_capacity = pixels;
			job.reference1_auxiliary_capacity = auxiliary_size(&ctx->capture);
			job.reference1_frame_id = buffer->index;
		} else {
			job.reference_address = 0x43000000;
			job.reference_auxiliary_address = job.reference_address + pixels;
			job.reference_capacity = pixels;
			job.reference_auxiliary_capacity = auxiliary_size(&ctx->capture);
			job.reference_frame_id = buffer->index;
		}
	}
	/* Reject unsupported controls before powering/mapping the device. */
	ret = w202_h264_build_ipb(&job, config);
	if (ret || atomic_read(&ctx->abort)) {
		if (!ret)
			ret = -ECANCELED;
		goto done;
	}
	ret = w202_vpu_native_begin(video->dev, &native);
	if (ret)
		goto done;
	ret = w202_vpu_native_import(native, input, &input_map);
	if (!ret)
		ret = w202_vpu_native_import(native, frame, &frame_map);
	for (i = 0; !ret && i < references; i++)
		ret = w202_vpu_native_import(native, reference[i], &reference_map[i]);
	if (!ret)
		ret = w202_vpu_native_validate_buffers(native, &input_map, &frame_map);
	if (ret)
		goto done;
	/* Only write padding after physical alias checks protect every reference. */
	ret = dma_buf_begin_cpu_access(input, DMA_BIDIRECTIONAL);
	if (ret)
		goto done;
	memset((u8 *)vb2_plane_vaddr(&src->vb2_buf, 0) + job.input_bytes,
	       0, W202_H264_BSM_PADDING);
	ret = dma_buf_end_cpu_access(input, DMA_BIDIRECTIONAL);
	if (ret)
		goto done;
	job.input_address = input_map.iova_addr;
	job.input_capacity = min_t(u64, input_map.size, job.input_capacity);
	job.output_address = frame_map.iova_addr;
	job.auxiliary_address = frame_map.iova_addr + pixels;
	if (reference[0]) {
		job.reference_address = reference_map[0].iova_addr;
		job.reference_auxiliary_address = reference_map[0].iova_addr + pixels;
	}
	if (reference[1]) {
		job.reference1_address = reference_map[1].iova_addr;
		job.reference1_auxiliary_address = reference_map[1].iova_addr + pixels;
	}
	ret = w202_h264_build_ipb(&job, config);
	if (!ret)
		ret = w202_vpu_native_execute(native, config, &ctx->abort);
done:
	if (native) {
		ended = w202_vpu_native_end(native);
		if (ended)
			ret = ended;
	}
	/* Failed reset leaves independent references and IOVAs in quarantine. */
	if (input)
		dma_buf_put(input);
	if (frame)
		dma_buf_put(frame);
	for (i = 0; i < ARRAY_SIZE(reference); i++)
		if (reference[i])
			dma_buf_put(reference[i]);
	kfree(config);
	if (ret)
		dev_dbg(video->dev, "V4L2 IPB job failed: %d\n", ret);
	if (!ret)
		WRITE_ONCE(buffer_state(&dst->vb2_buf)->reference, !!job.decode->nal_ref_idc);
	WRITE_ONCE(buffer_state(&dst->vb2_buf)->decoded, !ret);
	v4l2_m2m_buf_copy_metadata(src, dst, true);
	src->sequence = ctx->sequence;
	dst->sequence = ctx->sequence++;
	v4l2_ctrl_request_complete(request, &ctx->controls);
	v4l2_m2m_buf_done_and_job_finish(video->m2m, ctx->fh.m2m_ctx,
					ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
}

static void device_run(void *priv)
{
	struct w202_video_context *ctx = priv;

	schedule_work(&ctx->work);
}

static void job_abort(void *priv)
{
	struct w202_video_context *ctx = priv;

	atomic_set(&ctx->abort, 1);
	w202_vpu_native_wake(ctx->video->dev);
}

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run = device_run,
	.job_abort = job_abort,
};

static int request_validate(struct media_request *request)
{
	struct media_request_object *object;
	struct w202_video_context *ctx = NULL;
	struct v4l2_ctrl_handler *handler;
	static const u32 required[] = {
		V4L2_CID_STATELESS_H264_SPS, V4L2_CID_STATELESS_H264_PPS,
		V4L2_CID_STATELESS_H264_SLICE_PARAMS, V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	};
	unsigned int i;
	int ret = 0;

	if (vb2_request_buffer_cnt(request) != 1)
		return -EINVAL;
	list_for_each_entry(object, &request->objects, list) {
		struct vb2_buffer *buffer;

		if (!vb2_request_object_is_buffer(object))
			continue;
		buffer = container_of(object, struct vb2_buffer, req_obj);
		if (buffer->vb2_queue->ops != &queue_ops ||
		    !V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type))
			return -EINVAL;
		ctx = vb2_get_drv_priv(buffer->vb2_queue);
		break;
	}
	if (!ctx)
		return -ENOENT;
	handler = v4l2_ctrl_request_hdl_find(request, &ctx->controls);
	if (!handler)
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(required); i++)
		if (!v4l2_ctrl_request_hdl_ctrl_find(handler, required[i])) {
			ret = -ENOENT;
			break;
		}
	v4l2_ctrl_request_hdl_put(handler);
	if (ret)
		return ret;
	return vb2_request_validate(request);
}

static const struct media_device_ops media_ops = {
	.req_validate = request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int controls_init(struct w202_video_context *ctx)
{
	static const struct v4l2_ctrl_config configs[] = {
		{.id = V4L2_CID_STATELESS_H264_SPS},
		{.id = V4L2_CID_STATELESS_H264_PPS},
		{.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS},
		{.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS},
		{.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX},
		{.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		 .max = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		 .def = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED},
		{.id = V4L2_CID_STATELESS_H264_START_CODE,
		 .max = V4L2_STATELESS_H264_START_CODE_NONE,
		 .def = V4L2_STATELESS_H264_START_CODE_NONE},
		{.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		 .min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
		 .max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		 .menu_skip_mask = BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
		 .def = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE},
	};
	struct v4l2_ctrl **controls[] = {
		&ctx->sps, &ctx->pps, &ctx->slice, &ctx->decode, &ctx->scaling,
	};
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(&ctx->controls, ARRAY_SIZE(configs));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(configs); i++) {
		struct v4l2_ctrl *ctrl = v4l2_ctrl_new_custom(&ctx->controls, &configs[i], NULL);

		if (i < ARRAY_SIZE(controls))
			*controls[i] = ctrl;
		else if (ctrl && configs[i].id == V4L2_CID_MPEG_VIDEO_H264_PROFILE)
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}
	return ctx->controls.error;
}

static int video_open(struct file *file)
{
	struct w202_vpu_video *video = video_drvdata(file);
	struct w202_video_context *ctx;
	int ret;

	if (mutex_lock_interruptible(&video->lock))
		return -ERESTARTSYS;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->video = video;
	INIT_WORK(&ctx->work, decode_work);
	atomic_set(&ctx->abort, 0);
	v4l2_fh_init(&ctx->fh, &video->video);
	ctx->output = (struct v4l2_pix_format){.width = 64, .height = 48};
	format_fill(&ctx->output, true);
	ctx->capture = ctx->output;
	format_fill(&ctx->capture, false);
	ret = controls_init(ctx);
	if (ret)
		goto free_context;
	ctx->fh.ctrl_handler = &ctx->controls;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(video->m2m, ctx, queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto free_context;
	}
	v4l2_fh_add(&ctx->fh, file);
	mutex_unlock(&video->lock);
	return 0;
free_context:
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
unlock:
	mutex_unlock(&video->lock);
	return ret;
}

static int video_release(struct file *file)
{
	struct w202_video_context *ctx = file_context(file);
	struct w202_vpu_video *video = ctx->video;

	mutex_lock(&video->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	cancel_work_sync(&ctx->work);
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&video->lock);
	return 0;
}

static const struct v4l2_file_operations video_fops = {
	.owner = THIS_MODULE,
	.open = video_open,
	.release = video_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops video_ioctls = {
	.vidioc_querycap = video_querycap,
	.vidioc_enum_fmt_vid_out = video_enum_format,
	.vidioc_enum_fmt_vid_cap = video_enum_format,
	.vidioc_enum_framesizes = video_enum_framesizes,
	.vidioc_g_fmt_vid_out = video_get_format,
	.vidioc_g_fmt_vid_cap = video_get_format,
	.vidioc_try_fmt_vid_out = video_try_format,
	.vidioc_try_fmt_vid_cap = video_try_format,
	.vidioc_s_fmt_vid_out = video_set_format,
	.vidioc_s_fmt_vid_cap = video_set_format,
	.vidioc_g_selection = video_get_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

struct w202_vpu_video *w202_vpu_video_register(struct device *dev)
{
	struct w202_vpu_video *video;
	int ret;

	video = kzalloc(sizeof(*video), GFP_KERNEL);
	if (!video)
		return ERR_PTR(-ENOMEM);
	video->dev = dev;
	mutex_init(&video->lock);
	video->media.dev = dev;
	strscpy(video->media.model, W202_VIDEO_NAME, sizeof(video->media.model));
	strscpy(video->media.bus_info, "platform:w202-vpu", sizeof(video->media.bus_info));
	media_device_init(&video->media);
	video->media.ops = &media_ops;
	video->v4l2.mdev = &video->media;
	ret = v4l2_device_register(dev, &video->v4l2);
	if (ret)
		goto free_video;
	video->m2m = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(video->m2m)) {
		ret = PTR_ERR(video->m2m);
		goto unregister_v4l2;
	}
	video->video = (struct video_device){
		.v4l2_dev = &video->v4l2,
		.vfl_dir = VFL_DIR_M2M,
		.fops = &video_fops,
		.ioctl_ops = &video_ioctls,
		.release = video_device_release_empty,
		.lock = &video->lock,
		.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
	};
	strscpy(video->video.name, W202_VIDEO_NAME, sizeof(video->video.name));
	video_set_drvdata(&video->video, video);
	ret = video_register_device(&video->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_m2m;
	ret = v4l2_m2m_register_media_controller(video->m2m, &video->video,
					       MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto unregister_video;
	ret = media_device_register(&video->media);
	if (ret)
		goto unregister_mc;
	dev_info(dev, "experimental Baseline/Main IP Request API at /dev/video%d\n", video->video.num);
	return video;
unregister_mc:
	v4l2_m2m_unregister_media_controller(video->m2m);
unregister_video:
	video_unregister_device(&video->video);
release_m2m:
	v4l2_m2m_release(video->m2m);
unregister_v4l2:
	v4l2_device_unregister(&video->v4l2);
free_video:
	media_device_cleanup(&video->media);
	kfree(video);
	return ERR_PTR(ret);
}

void w202_vpu_video_unregister(struct w202_vpu_video *video)
{
	media_device_unregister(&video->media);
	v4l2_m2m_unregister_media_controller(video->m2m);
	video_unregister_device(&video->video);
	v4l2_m2m_release(video->m2m);
	v4l2_device_unregister(&video->v4l2);
	media_device_cleanup(&video->media);
	kfree(video);
}
