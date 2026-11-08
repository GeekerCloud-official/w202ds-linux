// SPDX-License-Identifier: GPL-2.0-only
/* Qogirn6Pro H.264 controls to hardware parameters. Register meanings are
 * reconstructed from observed factory jobs; MMIO offsets are relative to the
 * decoder resource, not the factory library's artificial 0x61200000 bias.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "w202_vpu_h264.h"
#include "w202_vpu_h264_cabac.h"

static void emit(struct w202_h264_config *c, __u32 offset, __u32 value)
{
	if (c->count < W202_H264_MAX_REGS)
		c->regs[c->count++] = (struct w202_h264_reg){offset, value};
	else
		c->count = W202_H264_MAX_REGS + 1;
}

static int valid_span(__u64 address, __u64 capacity, __u64 needed)
{
	return address >= 0x20000000 && !(address & 15) && capacity >= needed &&
	       address < 0x60000000 && capacity <= 0x60000000 - address;
}

static void emit_scaling(struct w202_h264_config *c, __u32 offset,
			 const __u8 *weights, unsigned int count)
{
	unsigned int i;

	/* V4L2 supplies resolved raster-order lists. The hardware consumes four
	 * consecutive coefficients per little-endian word, without prescaling. */
	for (i = 0; i < count; i += 4)
		emit(c, offset + i, (__u32)weights[i] |
		     ((__u32)weights[i + 1] << 8) |
		     ((__u32)weights[i + 2] << 16) |
		     ((__u32)weights[i + 3] << 24));
}

static int build_picture(const struct w202_h264_job *j, struct w202_h264_config *c,
			 int allow_b)
{
	/* CAVLC token symbol RAM, measured at the CPU table-transfer boundary.
	 * Padding is explicitly zero; the hardware consumes 18 16-byte groups. */
	static const __u32 cavlc[] = {
		0x0041c701, 0x4100c641, 0x8200c500, 0x0000c400,
		0x42c4c302, 0x01c38242, 0xc3824182, 0xc3820000,
		0xc5c64503, 0x83838543, 0xc4434483, 0xc40184c3,
		0xc6c54304, 0x84c5c844, 0x43428384, 0x024242c4,
		0xc7c70305, 0x85848745, 0x44444785, 0x030202c5,
		0xc8c8c906, 0x86858646, 0x45454686, 0x040301c6,
		0xc9050707, 0x87860647, 0x46468987, 0x050405c7,
		0x00c9ca08, 0x00878848, 0x00474888, 0x000604c8,
		0x08cbcc09, 0x89898b49, 0x48494a89, 0x070809c9,
		0xcacacb0a, 0x88888a4a, 0x4748498a, 0x060708ca,
		0xcc0b0c0b, 0x8b8b8d4b, 0x4a4b4c8b, 0x0a0a0bcb,
		0xcbcccd0c, 0x8a8a8c4c, 0x494a4b8c, 0x09090acc,
		0xcece4f0d, 0x8d8d0e4d, 0x4c4dce8d, 0x0c0d8ecd,
		0xcdcd4e0e, 0x8c8c0d4e, 0x4b4c4d8e, 0x0b0c4dce,
		0xd04f500f, 0x8f0f0f4f, 0x4f8fcf8f, 0x0e4e8fcf,
		0xcf8ed010, 0x8e8e9050, 0x4e0e1090, 0x0d0e00d0,
		0x10d00000, 0x90900000, 0x50500000, 0x0f100000,
		0x4dcf0000, 0, 0, 0,
	};
	const struct v4l2_ctrl_h264_sps *s;
	const struct v4l2_ctrl_h264_pps *p;
	const struct v4l2_ctrl_h264_slice_params *sl;
	const struct v4l2_ctrl_h264_decode_params *d;
	const struct v4l2_h264_dpb_entry *ref = NULL;
	const struct v4l2_h264_dpb_entry *ref1 = NULL;
	__u32 width_mb, height_mb, mbs, width, height, control;
	__u64 y_size;
	__u64 reference, reference_aux;
	int qp, inter, bidir, cabac;
	unsigned int i;

	if (!j || !c || !j->sps || !j->pps || !j->slice || !j->decode)
		return -EINVAL;
	s = j->sps; p = j->pps; sl = j->slice; d = j->decode;
	bidir = sl->slice_type == V4L2_H264_SLICE_TYPE_B;
	inter = sl->slice_type == V4L2_H264_SLICE_TYPE_P || bidir;
	cabac = !!(p->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE);
	if ((s->profile_idc != 66 && s->profile_idc != 77 && s->profile_idc != 100) ||
	    (bidir && (!allow_b || s->profile_idc == 66 || d->nal_ref_idc)) ||
	    (bidir && !(sl->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)) ||
	    (s->profile_idc == 66 && cabac) || s->chroma_format_idc != 1 ||
	    s->bit_depth_luma_minus8 || s->bit_depth_chroma_minus8 ||
	    !(s->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    (s->flags & (V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE |
			 V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS |
			 V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)) ||
	    p->num_slice_groups_minus1 || p->weighted_bipred_idc ||
	    (p->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) ||
	    (s->profile_idc != 100 && (p->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)) ||
	    (sl->slice_type != V4L2_H264_SLICE_TYPE_I && !inter) || sl->first_mb_in_slice ||
	    (d->flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC |
			 V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)) ||
	    (inter && ((d->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) ||
		       !!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_PFRAME) != !bidir ||
		       !!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_BFRAME) != bidir)) ||
	    (!inter && ((d->flags & V4L2_H264_DECODE_PARAM_FLAG_PFRAME) ||
			(d->flags & V4L2_H264_DECODE_PARAM_FLAG_BFRAME) ||
			!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) ||
			d->frame_num || d->top_field_order_cnt != d->bottom_field_order_cnt)))
		return -EOPNOTSUPP;
	if (p->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		const __u8 *weights;

		if (!j->scaling)
			return -EINVAL;
		weights = &j->scaling->scaling_list_4x4[0][0];
		for (i = 0; i < sizeof(j->scaling->scaling_list_4x4); i++)
			if (!weights[i])
				return -EINVAL;
		if (p->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) {
			/* 4:2:0 uses only intra Y [0] and inter Y [1]. */
			weights = &j->scaling->scaling_list_8x8[0][0];
			for (i = 0; i < 2 * 64; i++)
				if (!weights[i])
					return -EINVAL;
		}
	}
	width_mb = (__u32)s->pic_width_in_mbs_minus1 + 1;
	height_mb = (__u32)s->pic_height_in_map_units_minus1 + 1;
	if (width_mb > 120 || height_mb > 68 ||
	    p->seq_parameter_set_id != s->seq_parameter_set_id ||
	    s->seq_parameter_set_id > 31)
		return -EINVAL;
	width = width_mb * 16; height = height_mb * 16; mbs = width_mb * height_mb;
	y_size = (__u64)width * height;
	qp = 26 + p->pic_init_qp_minus26 + sl->slice_qp_delta;
	if (s->log2_max_frame_num_minus4 > 12 || s->pic_order_cnt_type > 2 ||
	    s->log2_max_pic_order_cnt_lsb_minus4 > 12 || s->max_num_ref_frames > 16 ||
	    (!inter && !d->nal_ref_idc) || d->nal_ref_idc > 3 || qp < 0 || qp > 51 ||
	    sl->colour_plane_id || sl->reserved ||
	    (sl->flags & ~(bidir ? V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED : 0)) ||
	    (cabac && sl->cabac_init_idc > 2) ||
	    sl->disable_deblocking_filter_idc > 2 ||
	    sl->slice_alpha_c0_offset_div2 < -6 || sl->slice_alpha_c0_offset_div2 > 6 ||
	    sl->slice_beta_offset_div2 < -6 || sl->slice_beta_offset_div2 > 6 ||
	    p->chroma_qp_index_offset < -12 || p->chroma_qp_index_offset > 12 ||
	    p->second_chroma_qp_index_offset < -12 || p->second_chroma_qp_index_offset > 12 ||
	    !j->input_bytes || j->input_bytes > 8 * 1024 * 1024 ||
	    !j->nalu_bytes || j->nalu_bytes > j->input_bytes ||
	    sl->header_bit_size < 8 || sl->header_bit_size > j->nalu_bytes * 8 ||
	    !valid_span(j->input_address, j->input_capacity,
			(__u64)j->input_bytes + W202_H264_BSM_PADDING) ||
	    !valid_span(j->output_address, j->output_capacity, y_size * 3 / 2) ||
	    !valid_span(j->auxiliary_address, j->auxiliary_capacity, mbs * 80 + 1024))
		return -EINVAL;
	if (inter) {
		unsigned int index = sl->ref_pic_list0[0].index;

		if (sl->num_ref_idx_l0_active_minus1 || index >= V4L2_H264_NUM_DPB_ENTRIES ||
		    sl->ref_pic_list0[0].fields != V4L2_H264_FRAME_REF ||
		    j->reference_frame_id > 31)
			return -EOPNOTSUPP;
		ref = &d->dpb[index];
		if ((ref->flags & (V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)) !=
		    (V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
		    ref->fields != V4L2_H264_FRAME_REF ||
		    (ref->flags & (V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM |
				  V4L2_H264_DPB_ENTRY_FLAG_FIELD)) ||
		    ref->top_field_order_cnt != ref->bottom_field_order_cnt ||
		    d->top_field_order_cnt != d->bottom_field_order_cnt ||
		    !valid_span(j->reference_address, j->reference_capacity, y_size * 3 / 2) ||
		    !valid_span(j->reference_auxiliary_address, j->reference_auxiliary_capacity,
				mbs * 80 + 1024))
			return -EINVAL;
	}
	if (bidir) {
		unsigned int index = sl->ref_pic_list1[0].index;

		if (sl->num_ref_idx_l1_active_minus1 || index >= V4L2_H264_NUM_DPB_ENTRIES ||
		    sl->ref_pic_list1[0].fields != V4L2_H264_FRAME_REF ||
		    j->reference1_frame_id > 31)
			return -EOPNOTSUPP;
		ref1 = &d->dpb[index];
		if ((ref1->flags & (V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)) !=
		    (V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
		    ref1->fields != V4L2_H264_FRAME_REF ||
		    (ref1->flags & (V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM |
				   V4L2_H264_DPB_ENTRY_FLAG_FIELD)) ||
		    ref1->top_field_order_cnt != ref1->bottom_field_order_cnt ||
		    !valid_span(j->reference1_address, j->reference1_capacity, y_size * 3 / 2) ||
		    !valid_span(j->reference1_auxiliary_address, j->reference1_auxiliary_capacity,
				mbs * 80 + 1024))
			return -EINVAL;
	}
	memset(c, 0, sizeof(*c));
	c->width = width; c->height = height;
	c->stream_address = (__u32)j->input_address;
	c->stream_bytes = j->input_bytes; c->header_bits = sl->header_bit_size;
	emit(c, 0x1080, (__u32)(j->input_address >> 4));
	emit(c, 0x1084, (__u32)(j->input_address >> 4));
	emit(c, 0x1088, (__u32)(j->output_address >> 4));
	emit(c, 0x108c, (__u32)((j->output_address + y_size) >> 4));
	emit(c, 0x1090, (__u32)(j->auxiliary_address >> 4));
	/* Linear NV12, 8-bit samples, frame picture, hardware AVC mode 4. */
	emit(c, 0x1020, 0x11000004 | ((__u32)(inter + bidir) << 16));
	emit(c, 0x1024, (width_mb & 255) | ((height_mb & 255) << 8));
	emit(c, 0x1034, j->nalu_bytes);
	emit(c, 0x103c, 0x80000000U | ((__u32)qp << 25) | mbs);
	emit(c, 0x1040, 0);
	emit(c, 0x1044, (s->profile_idc == 66 ? 0 : 0x80000000) |
	     ((p->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) ? 0x20000000 : 0));
	control = ((__u32)sl->disable_deblocking_filter_idc << 19) |
		  ((__u32)!!(s->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) << 12) |
		  ((__u32)!!(p->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) << 13) |
		  ((__u32)!!(p->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) << 10) |
		  ((__u32)cabac << 14) |
		  ((__u32)!!(sl->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED) << 11) |
		  /* Standard controls carry offset_div2; registers carry the offset. */
		  (((__u32)(sl->slice_beta_offset_div2 * 2) & 31) << 5) |
		  ((__u32)(sl->slice_alpha_c0_offset_div2 * 2) & 31);
	emit(c, 0x1048, control);
	/* Active reference counts and count-minus-one syntax fields. */
	emit(c, 0x104c, (bidir ? 0x2100000 : inter ? 0x1f8000 : 0xffc00) |
			(((__u32)p->second_chroma_qp_index_offset & 31) << 5) |
			((__u32)p->chroma_qp_index_offset & 31));
	for (i = 0x1050; i <= 0x107c; i += 4) emit(c, i, 0);
	for (i = 0x1160; i <= 0x1170; i += 4) emit(c, i, 0);
	/* Clients may use a common POC origin, including for the IDR picture. */
	emit(c, 0x1050, (__u32)d->top_field_order_cnt);
	emit(c, 0x1054, (__u32)d->top_field_order_cnt);
	emit(c, 0x1058, (__u32)d->bottom_field_order_cnt);
	if (inter) {
		/* 6-bit picture identities; 0x3f denotes an unused entry. */
		for (i = 0x1064; i <= 0x107c; i += 4) emit(c, i, 0xffffffff);
		for (i = 0x1160; i <= 0x1170; i += 4) emit(c, i, 0xffffffff);
		emit(c, 0x1068, 0xffffffc0 | j->reference_frame_id);
		if (bidir)
			emit(c, 0x1074, 0xffffffc0 | j->reference1_frame_id);
	}
	emit(c, 0x1180, width | (height_mb << 20));
	for (i = 0x1184; i <= 0x1198; i += 4) emit(c, i, 0);
	if (p->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		emit_scaling(c, 0x1400, &j->scaling->scaling_list_4x4[3][0], 3 * 16);
		emit_scaling(c, 0x1440, &j->scaling->scaling_list_4x4[0][0], 3 * 16);
		if (p->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) {
			emit_scaling(c, 0x1480, j->scaling->scaling_list_8x8[1], 64);
			emit_scaling(c, 0x14c0, j->scaling->scaling_list_8x8[0], 64);
		}
	}
	if (cabac) {
		unsigned int table = inter ? sl->cabac_init_idc + 1 : 0;

		for (i = 0; i < 454; i++) {
			const __s8 *mn = w202_h264_cabac_init[table][i];
			int pre = ((mn[0] * qp) >> 4) + mn[1];

			pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
			emit(c, 0x1800 + i * 4,
			     pre <= 63 ? (63 - pre) * 2 : (pre - 64) * 2 + 1);
		}
		emit(c, 0x1c38, 0); /* termination/bypass context */
	} else {
		for (i = 0; i < sizeof(cavlc) / sizeof(cavlc[0]); i++)
			emit(c, 0x3000 + i * 4, cavlc[i]);
	}
	for (i = 0; i < 0x88; i += 4) { emit(c, 0x1200 + i, 0); emit(c, 0x2000 + i, 0); }
	if (inter) {
		emit(c, 0x1200, (__u32)ref->top_field_order_cnt);
		if (bidir)
			emit(c, 0x1240, (__u32)ref1->top_field_order_cnt);
		emit(c, 0x1280, bidir ? 0xfffefffe : 0xfffffffe);
		emit(c, 0x2080, 0xffffffff);
	}
	/* Every programmed address belongs to this job, including unused entries. */
	for (i = 0; i < 32; i++) {
		reference = inter && !i ? j->reference_address : j->output_address;
		reference_aux = inter && !i ? j->reference_auxiliary_address : j->auxiliary_address;
		emit(c, 0x2400 + i * 4, (__u32)(reference >> 4));
		emit(c, 0x2500 + i * 4, (__u32)((reference + y_size) >> 4));
		emit(c, 0x2600 + i * 4, (__u32)(reference_aux >> 4));
		reference = bidir && !i ? j->reference1_address : j->output_address;
		reference_aux = bidir && !i ? j->reference1_auxiliary_address : j->auxiliary_address;
		emit(c, 0x2480 + i * 4, (__u32)(reference >> 4));
		emit(c, 0x2580 + i * 4, (__u32)((reference + y_size) >> 4));
		emit(c, 0x2680 + i * 4, (__u32)(reference_aux >> 4));
	}
	return c->count <= W202_H264_MAX_REGS ? 0 : -E2BIG;
}

int w202_h264_build_ip(const struct w202_h264_job *job, struct w202_h264_config *config)
{
	return build_picture(job, config, 0);
}

int w202_h264_build_ipb(const struct w202_h264_job *job, struct w202_h264_config *config)
{
	return build_picture(job, config, 1);
}

int w202_h264_build_idr(const struct w202_h264_job *job, struct w202_h264_config *config)
{
	if (!job || !job->slice || job->slice->slice_type != V4L2_H264_SLICE_TYPE_I)
		return -EOPNOTSUPP;
	return w202_h264_build_ip(job, config);
}
