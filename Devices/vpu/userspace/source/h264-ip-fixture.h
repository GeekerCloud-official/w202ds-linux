/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic parser for generated, single-reference Baseline IP fixtures.
 * No list reordering, weighted prediction, MMCO, fields or multi-slice frames.
 */
#ifndef W202_H264_IP_FIXTURE_H
#define W202_H264_IP_FIXTURE_H
#include "h264-idr-fixture.h"
#define W202_IP_FIXTURE_FRAMES 32
struct h264_ip_fixture {
	unsigned int count;
	struct h264_fixture frame[W202_IP_FIXTURE_FRAMES];
};

static int h264_parse_ip(const unsigned char *stream, size_t length,
			 struct h264_ip_fixture *fixture)
{
	unsigned char *rbsp = malloc(length);
	int result = -1, have_idr = 0;
	size_t first_end = length;

	if (!rbsp)
		return -1;
	memset(fixture, 0, sizeof(*fixture));
	for (size_t pos = 0; pos < length;) {
		unsigned int prefix = h264_startcode(stream + pos, length - pos);
		if (!prefix)
			goto out;
		size_t start = pos + prefix, end = start, prefix_start = pos;
		while (end < length && !h264_startcode(stream + end, length - end))
			end++;
		pos = end;
		while (end > start && !stream[end - 1])
			end--;
		if (end == start || (stream[start] & 0x80))
			goto out;
		unsigned int type = stream[start] & 31;
		if (type == 5) {
			if (have_idr++)
				goto out;
			continue;
		}
		if (type != 1)
			continue;
		if (!have_idr || fixture->count >= W202_IP_FIXTURE_FRAMES)
			goto out;
		if (!fixture->count) {
			first_end = prefix_start;
			if (h264_parse_idr(stream, first_end, &fixture->frame[0]))
				goto out;
			fixture->count = 1;
		}
		struct h264_fixture *f = &fixture->frame[fixture->count];
		const struct h264_fixture *first = &fixture->frame[0];
		const struct h264_fixture *previous = &fixture->frame[fixture->count - 1];
		*f = *first;
		memset(&f->slice, 0, sizeof(f->slice));
		memset(&f->decode, 0, sizeof(f->decode));
		unsigned int zeros = 0;
		size_t size = 0;
		for (size_t i = start; i < end; i++) {
			if (zeros == 2 && stream[i] == 3) {
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
		if (slice_type > 9 || slice_type % 5 != V4L2_H264_SLICE_TYPE_P ||
		    sl->first_mb_in_slice || pps_id != p->pic_parameter_set_id ||
		    s->pic_order_cnt_type != 2 ||
		    (p->flags & (V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
				 V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)))
			goto out;
		sl->slice_type = V4L2_H264_SLICE_TYPE_P;
		d->flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
		d->nal_ref_idc = (stream[start] >> 5) & 3;
		if (!d->nal_ref_idc)
			goto out;
		d->frame_num = h264_get(&bits, s->log2_max_frame_num_minus4 + 4);
		d->top_field_order_cnt = fixture->count * 2;
		d->bottom_field_order_cnt = d->top_field_order_cnt;
		uint32_t refs = p->num_ref_idx_l0_default_active_minus1;
		if (h264_get(&bits, 1))
			refs = h264_ue(&bits);
		if (refs || h264_get(&bits, 1)) /* ref_pic_list_modification_flag_l0 */
			goto out;
		if (h264_get(&bits, 1)) /* adaptive_ref_pic_marking_mode_flag */
			goto out;
		d->dec_ref_pic_marking_bit_size = 1;
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
		sl->ref_pic_list0[0] = (struct v4l2_h264_reference){.fields = V4L2_H264_FRAME_REF};
		d->dpb[0] = (struct v4l2_h264_dpb_entry){
			.reference_ts = fixture->count,
			.pic_num = previous->decode.frame_num,
			.frame_num = previous->decode.frame_num,
			.fields = V4L2_H264_FRAME_REF,
			.top_field_order_cnt = previous->decode.top_field_order_cnt,
			.bottom_field_order_cnt = previous->decode.bottom_field_order_cnt,
			.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
		};
		f->nal = stream + start;
		f->nal_size = end - start;
		f->rbsp_size = size;
		fixture->count++;
	}
	if (!fixture->count) {
		if (h264_parse_idr(stream, first_end, &fixture->frame[0]))
			goto out;
		fixture->count = 1;
	}
	result = 0;
out:
	free(rbsp);
	return result;
}
#endif
