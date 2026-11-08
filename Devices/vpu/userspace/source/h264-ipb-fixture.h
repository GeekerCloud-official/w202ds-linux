/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic parser for progressive Main IPB fixtures, with POC type 0,
 * non-reference B pictures and one short-term entry in each active list.
 * No list modification, weighting, MMCO, fields or multiple slices.
 * This parser is never part of the kernel driver.
 */
#ifndef W202_H264_IPB_FIXTURE_H
#define W202_H264_IPB_FIXTURE_H
#include "h264-idr-fixture.h"
#define W202_IPB_FIXTURE_FRAMES 32
#define W202_IPB_NO_REFERENCE W202_IPB_FIXTURE_FRAMES
struct h264_ipb_fixture {
	unsigned int count;
	struct h264_fixture frame[W202_IPB_FIXTURE_FRAMES];
	unsigned int ref0[W202_IPB_FIXTURE_FRAMES];
	unsigned int ref1[W202_IPB_FIXTURE_FRAMES];
	unsigned int display_order[W202_IPB_FIXTURE_FRAMES];
};

static void h264_ipb_reference(struct v4l2_ctrl_h264_decode_params *decode,
			       unsigned int entry, const struct h264_fixture *reference,
			       unsigned int index)
{
	decode->dpb[entry] = (struct v4l2_h264_dpb_entry){
		.reference_ts = (uint64_t)(index + 1) * 1000000,
		.pic_num = reference->decode.frame_num,
		.frame_num = reference->decode.frame_num,
		.fields = V4L2_H264_FRAME_REF,
		.top_field_order_cnt = reference->decode.top_field_order_cnt,
		.bottom_field_order_cnt = reference->decode.bottom_field_order_cnt,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
	};
}

static int h264_parse_ipb(const unsigned char *stream, size_t length,
			  struct h264_ipb_fixture *fixture)
{
	unsigned char *rbsp = malloc(length);
	unsigned int latest = W202_IPB_NO_REFERENCE, past = W202_IPB_NO_REFERENCE;
	int result = -1, previous_lsb = 0, previous_msb = 0;

	if (!rbsp)
		return -1;
	memset(fixture, 0, sizeof(*fixture));
	for (unsigned int i = 0; i < W202_IPB_FIXTURE_FRAMES; i++)
		fixture->ref0[i] = fixture->ref1[i] = W202_IPB_NO_REFERENCE;
	for (size_t pos = 0; pos < length;) {
		unsigned int prefix = h264_startcode(stream + pos, length - pos);
		if (!prefix)
			goto out;
		size_t start = pos + prefix, end = start;
		while (end < length && !h264_startcode(stream + end, length - end))
			end++;
		pos = end;
		while (end > start && !stream[end - 1])
			end--;
		if (end == start || (stream[start] & 0x80))
			goto out;
		unsigned int type = stream[start] & 31;
		if (type == 5) {
			if (fixture->count || h264_parse_idr(stream, pos, &fixture->frame[0]))
				goto out;
			const struct h264_fixture *f = &fixture->frame[0];
			if ((f->sps.profile_idc != 77 && f->sps.profile_idc != 100) || f->sps.pic_order_cnt_type ||
			    f->decode.frame_num || f->decode.top_field_order_cnt ||
			    f->decode.bottom_field_order_cnt || f->slice.first_mb_in_slice ||
			    (f->pps.flags & (V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
					    V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)) ||
			    f->pps.weighted_bipred_idc)
				goto out;
			fixture->count = 1;
			latest = 0;
			continue;
		}
		if (type != 1) {
			if (type != 6 && type != 9 && type != 12 &&
			    (fixture->count || (type != 7 && type != 8)))
				goto out;
			continue;
		}
		unsigned int current = fixture->count;
		if (!current || current >= W202_IPB_FIXTURE_FRAMES)
			goto out;
		struct h264_fixture *f = &fixture->frame[current];
		*f = fixture->frame[0];
		memset(&f->slice, 0, sizeof(f->slice));
		memset(&f->decode, 0, sizeof(f->decode));
		unsigned int zeros = 0;
		size_t size = 0;
		for (size_t i = start; i < end; i++) {
			if (zeros >= 2 && stream[i] == 3) {
				if (i + 1 == end || stream[i + 1] > 3)
					goto out;
				zeros = 0;
				continue;
			}
			rbsp[size++] = stream[i];
			zeros = stream[i] ? 0 : zeros + 1;
		}
		struct h264_bits bits = {.p = rbsp, .size = size, .bit = 8};
		struct v4l2_ctrl_h264_slice_params *sl = &f->slice;
		struct v4l2_ctrl_h264_decode_params *d = &f->decode;
		const struct v4l2_ctrl_h264_sps *s = &f->sps;
		const struct v4l2_ctrl_h264_pps *p = &f->pps;
		sl->first_mb_in_slice = h264_ue(&bits);
		uint32_t slice_type = h264_ue(&bits), pps_id = h264_ue(&bits);
		if (slice_type > 9 ||
		    (slice_type % 5 != V4L2_H264_SLICE_TYPE_P && slice_type % 5 != V4L2_H264_SLICE_TYPE_B) ||
		    sl->first_mb_in_slice || pps_id != p->pic_parameter_set_id)
			goto out;
		sl->slice_type = slice_type % 5;
		int bidir = sl->slice_type == V4L2_H264_SLICE_TYPE_B;
		d->flags = bidir ? V4L2_H264_DECODE_PARAM_FLAG_BFRAME : V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
		d->nal_ref_idc = (stream[start] >> 5) & 3;
		if (!!d->nal_ref_idc == bidir)
			goto out;
		d->frame_num = h264_get(&bits, s->log2_max_frame_num_minus4 + 4);
		size_t poc_start = bits.bit;
		int max_lsb = 1 << (s->log2_max_pic_order_cnt_lsb_minus4 + 4);
		int lsb = h264_get(&bits, s->log2_max_pic_order_cnt_lsb_minus4 + 4);
		int msb = previous_msb;
		if (lsb < previous_lsb && previous_lsb - lsb >= max_lsb / 2)
			msb += max_lsb;
		else if (lsb > previous_lsb && lsb - previous_lsb > max_lsb / 2)
			msb -= max_lsb;
		d->pic_order_cnt_lsb = lsb;
		d->top_field_order_cnt = msb + lsb;
		if (p->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			d->delta_pic_order_cnt_bottom = h264_se(&bits);
		d->bottom_field_order_cnt = d->top_field_order_cnt + d->delta_pic_order_cnt_bottom;
		d->pic_order_cnt_bit_size = bits.bit - poc_start;
		if (d->top_field_order_cnt != d->bottom_field_order_cnt)
			goto out;
		if (bidir && h264_get(&bits, 1))
			sl->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
		uint32_t refs0 = p->num_ref_idx_l0_default_active_minus1;
		uint32_t refs1 = p->num_ref_idx_l1_default_active_minus1;
		if (h264_get(&bits, 1)) {
			refs0 = h264_ue(&bits);
			if (bidir)
				refs1 = h264_ue(&bits);
		}
		if (refs0 || (bidir && refs1) || h264_get(&bits, 1) ||
		    (bidir && h264_get(&bits, 1))) /* ref_pic_list_modification */
			goto out;
		if (d->nal_ref_idc) {
			if (h264_get(&bits, 1)) /* adaptive_ref_pic_marking_mode_flag */
				goto out;
			d->dec_ref_pic_marking_bit_size = 1;
		}
		if (p->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) {
			uint32_t init = h264_ue(&bits);
			if (init > 2)
				goto out;
			sl->cabac_init_idc = init;
		}
		int qp = h264_se(&bits);
		if (qp < -51 || qp > 51)
			goto out;
		sl->slice_qp_delta = qp;
		if (p->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
			uint32_t disable = h264_ue(&bits);
			if (disable > 2)
				goto out;
			sl->disable_deblocking_filter_idc = disable;
			if (disable != 1) {
				int alpha = h264_se(&bits), beta = h264_se(&bits);
				if (alpha < -6 || alpha > 6 || beta < -6 || beta > 6)
					goto out;
				sl->slice_alpha_c0_offset_div2 = alpha;
				sl->slice_beta_offset_div2 = beta;
			}
		}
		if (bits.bad)
			goto out;
		sl->header_bit_size = bits.bit;
		unsigned int l0 = bidir ? past : latest;
		if (l0 == W202_IPB_NO_REFERENCE)
			goto out;
		fixture->ref0[current] = l0;
		h264_ipb_reference(d, 0, &fixture->frame[l0], l0);
		sl->ref_pic_list0[0] = (struct v4l2_h264_reference){.fields = V4L2_H264_FRAME_REF};
		if (bidir) {
			if (d->top_field_order_cnt <= fixture->frame[past].decode.top_field_order_cnt ||
			    d->top_field_order_cnt >= fixture->frame[latest].decode.top_field_order_cnt)
				goto out;
			fixture->ref1[current] = latest;
			h264_ipb_reference(d, 1, &fixture->frame[latest], latest);
			sl->ref_pic_list1[0] = (struct v4l2_h264_reference){.fields = V4L2_H264_FRAME_REF, .index = 1};
		} else {
			if (d->top_field_order_cnt <= fixture->frame[latest].decode.top_field_order_cnt)
				goto out;
			past = latest;
			latest = current;
			previous_lsb = lsb;
			previous_msb = msb;
		}
		f->nal = stream + start;
		f->nal_size = end - start;
		f->rbsp_size = size;
		fixture->count++;
	}
	if (!fixture->count)
		goto out;
	/* The encoded stream is in decode order; software references are in
	 * display order. Require distinct POCs and record the output permutation. */
	for (unsigned int i = 0; i < fixture->count; i++) {
		unsigned int at = i;
		while (at && fixture->frame[fixture->display_order[at - 1]].decode.top_field_order_cnt >
				fixture->frame[i].decode.top_field_order_cnt) {
			fixture->display_order[at] = fixture->display_order[at - 1];
			at--;
		}
		fixture->display_order[at] = i;
	}
	for (unsigned int i = 1; i < fixture->count; i++)
		if (fixture->frame[fixture->display_order[i - 1]].decode.top_field_order_cnt ==
		    fixture->frame[fixture->display_order[i]].decode.top_field_order_cnt)
			goto out;
	result = 0;
out:
	free(rbsp);
	return result;
}
#endif
