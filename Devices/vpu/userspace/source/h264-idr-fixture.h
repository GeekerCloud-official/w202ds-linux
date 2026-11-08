/* SPDX-License-Identifier: GPL-2.0-only */
/* Restricted software parser for native-IDR and Request API test clients.
 * Accepts an 8-bit 4:2:0 Baseline/Main/High SPS/PPS and one IDR;
 * never used by the kernel driver.
 */
#ifndef W202_H264_IDR_FIXTURE_H
#define W202_H264_IDR_FIXTURE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../source/w202_vpu_h264.h"

struct h264_fixture {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_scaling_matrix scaling;
	struct v4l2_ctrl_h264_slice_params slice;
	struct v4l2_ctrl_h264_decode_params decode;
	const unsigned char *nal;
	size_t nal_size, rbsp_size;
	unsigned int crop_left, crop_top, width, height;
};
struct h264_bits {
	const unsigned char *p;
	size_t size, bit;
	int bad;
};
static uint32_t h264_get(struct h264_bits *b, unsigned int count)
{
	uint32_t v = 0;
	if (count > 32 || b->bit > b->size * 8 || count > b->size * 8 - b->bit) {
		b->bad = 1; return 0;
	}
	for (unsigned int i = 0; i < count; i++, b->bit++)
		v = (v << 1) | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1);
	return v;
}
static uint32_t h264_ue(struct h264_bits *b)
{
	unsigned int zeros = 0;
	while (!b->bad && !h264_get(b, 1)) {
		if (++zeros > 30) { b->bad = 1; return 0; }
	}
	return ((1U << zeros) - 1) + h264_get(b, zeros);
}
static int h264_se(struct h264_bits *b)
{
	uint32_t n = h264_ue(b);
	return n & 1 ? (int)((n + 1) / 2) : -(int)(n / 2);
}

static const uint8_t h264_default4[2][16] = {
	{6,13,20,28, 13,20,28,32, 20,28,32,37, 28,32,37,42},
	{10,14,20,24, 14,20,24,27, 20,24,27,30, 24,27,30,34},
};
static const uint8_t h264_default8[2][64] = {
	{6,10,13,16,18,23,25,27, 10,11,16,18,23,25,27,29,
	 13,16,18,23,25,27,29,31, 16,18,23,25,27,29,31,33,
	 18,23,25,27,29,31,33,36, 23,25,27,29,31,33,36,38,
	 25,27,29,31,33,36,38,40, 27,29,31,33,36,38,40,42},
	{9,13,15,17,19,21,22,24, 13,13,17,19,21,22,24,25,
	 15,17,19,21,22,24,25,27, 17,19,21,22,24,25,27,28,
	 19,21,22,24,25,27,28,30, 21,22,24,25,27,28,30,32,
	 22,24,25,27,28,30,32,33, 24,25,27,28,30,32,33,35},
};

/* Convert bitstream zigzag order to the raster order required by V4L2. */
static int h264_scaling_list(struct h264_bits *b, uint8_t *list,
			     unsigned int side, const uint8_t *defaults)
{
	int last = 8, next = 8;
	unsigned int index = 0;

	for (unsigned int diagonal = 0; diagonal < 2 * side - 1; diagonal++) {
		unsigned int low = diagonal < side ? 0 : diagonal - side + 1;
		unsigned int high = diagonal < side ? diagonal : side - 1;

		for (unsigned int j = low; j <= high; j++, index++) {
			unsigned int row = diagonal & 1 ? j : high - (j - low);
			unsigned int col = diagonal - row;

			if (next) {
				int delta = h264_se(b);

				if (b->bad || delta < -128 || delta > 127)
					return -1;
				next = (last + delta + 256) % 256;
				if (!index && !next) {
					memcpy(list, defaults, side * side);
					return 0;
				}
			}
			last = next ? next : last;
			list[row * side + col] = last;
		}
	}
	return b->bad ? -1 : 0;
}

static int h264_more_rbsp(const struct h264_bits *b)
{
	struct h264_bits tail = *b;

	if (tail.bit >= tail.size * 8 || !h264_get(&tail, 1))
		return 1;
	while (tail.bit < tail.size * 8)
		if (h264_get(&tail, 1))
			return 1;
	return 0;
}
static unsigned int h264_startcode(const unsigned char *p, size_t n)
{
	if (n >= 4 && !p[0] && !p[1] && !p[2] && p[3] == 1) return 4;
	if (n >= 3 && !p[0] && !p[1] && p[2] == 1) return 3;
	return 0;
}
static int h264_parse_idr(const unsigned char *stream, size_t length, struct h264_fixture *f)
{
	unsigned char *rbsp = malloc(length);
	int have_sps = 0, have_pps = 0, sps_scaling = 0, result = -1;
	if (!rbsp) return -1;
	memset(f, 0, sizeof(*f));
	memset(&f->scaling, 16, sizeof(f->scaling));
	for (size_t pos = 0; pos < length;) {
		unsigned int prefix = h264_startcode(stream + pos, length - pos);
		if (!prefix) goto out;
		size_t start = pos + prefix, end = start;
		while (end < length && !h264_startcode(stream + end, length - end)) end++;
		pos = end;
		while (end > start && !stream[end - 1]) end--; /* trailing_zero_8bits */
		if (end == start || (stream[start] & 0x80)) goto out;
		unsigned int nal_type = stream[start] & 31, zeros = 0;
		size_t size = 0;
		for (size_t i = start; i < end; i++) {
			if (zeros >= 2 && stream[i] == 3) {
				if (i + 1 == end || stream[i + 1] > 3) goto out;
				zeros = 0; continue;
			}
			rbsp[size++] = stream[i];
			zeros = stream[i] ? 0 : zeros + 1;
		}
		struct h264_bits b = {.p = rbsp, .size = size, .bit = 8};
		if (nal_type == 7) {
			if (have_sps++) goto out;
			struct v4l2_ctrl_h264_sps *s = &f->sps;
			s->profile_idc = h264_get(&b, 8);
			uint32_t constraint = h264_get(&b, 8);
			for (unsigned int i = 0; i < 6; i++) if (constraint & (128U >> i)) s->constraint_set_flags |= 1U << i;
			s->level_idc = h264_get(&b, 8);
			uint32_t sps_id = h264_ue(&b);
			if ((s->profile_idc != 66 && s->profile_idc != 77 && s->profile_idc != 100) || sps_id > 31) goto out;
			s->seq_parameter_set_id = sps_id; s->chroma_format_idc = 1;
			if (s->profile_idc == 100) {
				if (h264_ue(&b) != 1 || h264_ue(&b) || h264_ue(&b) || h264_get(&b, 1)) goto out;
				sps_scaling = h264_get(&b, 1);
				if (sps_scaling) {
					for (unsigned int i = 0; i < 8; i++) {
						uint8_t *list = i < 6 ? f->scaling.scaling_list_4x4[i] : f->scaling.scaling_list_8x8[i - 6];
						const uint8_t *defaults = i < 6 ? h264_default4[i / 3] : h264_default8[i - 6];
						if (h264_get(&b, 1)) {
							if (h264_scaling_list(&b, list, i < 6 ? 4 : 8, defaults)) goto out;
						} else if (i < 6 && i % 3) {
							memcpy(list, f->scaling.scaling_list_4x4[i - 1], 16);
						} else {
							memcpy(list, defaults, i < 6 ? 16 : 64);
						}
					}
				}
			}
			uint32_t log2 = h264_ue(&b), poc = h264_ue(&b);
			if (log2 > 12 || (poc != 0 && poc != 2)) goto out;
			s->log2_max_frame_num_minus4 = log2; s->pic_order_cnt_type = poc;
			if (!poc) {
				log2 = h264_ue(&b); if (log2 > 12) goto out;
				s->log2_max_pic_order_cnt_lsb_minus4 = log2;
			}
			uint32_t refs = h264_ue(&b); if (refs > 16) goto out;
			s->max_num_ref_frames = refs;
			if (h264_get(&b, 1)) s->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
			uint32_t w = h264_ue(&b), h = h264_ue(&b);
			if (w >= 120 || h >= 68 || !h264_get(&b, 1)) goto out;
			s->pic_width_in_mbs_minus1 = w; s->pic_height_in_map_units_minus1 = h;
			s->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
			if (h264_get(&b, 1)) s->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
			uint32_t left = 0, right = 0, top = 0, bottom = 0;
			if (h264_get(&b, 1)) { left = h264_ue(&b); right = h264_ue(&b); top = h264_ue(&b); bottom = h264_ue(&b); }
			w = (w + 1) * 16; h = (h + 1) * 16;
			if (left > w / 2 || right >= w / 2 - left || top > h / 2 || bottom >= h / 2 - top) goto out;
			f->crop_left = left * 2; f->crop_top = top * 2;
			f->width = w - 2 * (left + right); f->height = h - 2 * (top + bottom);
			/* VUI is not needed for reconstruction or the pixel comparison. */
		} else if (nal_type == 8) {
			if (!have_sps || have_pps++) goto out;
			struct v4l2_ctrl_h264_pps *p = &f->pps;
			uint32_t pps_id = h264_ue(&b), sps_id = h264_ue(&b);
			if (pps_id > 255 || sps_id != f->sps.seq_parameter_set_id) goto out;
			p->pic_parameter_set_id = pps_id; p->seq_parameter_set_id = sps_id;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
			if (h264_ue(&b)) goto out;
			uint32_t n0 = h264_ue(&b), n1 = h264_ue(&b);
			if (n0 > 31 || n1 > 31) goto out;
			p->num_ref_idx_l0_default_active_minus1 = n0; p->num_ref_idx_l1_default_active_minus1 = n1;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
			p->weighted_bipred_idc = h264_get(&b, 2);
			int qp = h264_se(&b), qs = h264_se(&b), chroma = h264_se(&b);
			if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || chroma < -12 || chroma > 12) goto out;
			p->pic_init_qp_minus26 = qp; p->pic_init_qs_minus26 = qs;
			p->chroma_qp_index_offset = chroma; p->second_chroma_qp_index_offset = chroma;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
			if (h264_get(&b, 1)) p->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
			if (sps_scaling) p->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
			if (h264_more_rbsp(&b)) {
				int transform8 = h264_get(&b, 1);

				if (transform8) p->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
				if (h264_get(&b, 1)) {
					p->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
					for (unsigned int i = 0; i < 6U + 2U * transform8; i++) {
						uint8_t *list = i < 6 ? f->scaling.scaling_list_4x4[i] : f->scaling.scaling_list_8x8[i - 6];
						const uint8_t *defaults = i < 6 ? h264_default4[i / 3] : h264_default8[i - 6];
						if (h264_get(&b, 1)) {
							if (h264_scaling_list(&b, list, i < 6 ? 4 : 8, defaults)) goto out;
						} else if (i < 6 && i % 3) {
							memcpy(list, f->scaling.scaling_list_4x4[i - 1], 16);
						} else if (!sps_scaling) {
							memcpy(list, defaults, i < 6 ? 16 : 64);
						}
					}
				}
				int second = h264_se(&b);
				if (second < -12 || second > 12) goto out;
				p->second_chroma_qp_index_offset = second;
			}
			if (b.bad || !h264_get(&b, 1)) goto out;
			while (b.bit < b.size * 8) if (h264_get(&b, 1)) goto out;
		} else if (nal_type == 5) {
			if (!have_sps || !have_pps || f->nal) goto out;
			struct v4l2_ctrl_h264_slice_params *sl = &f->slice;
			struct v4l2_ctrl_h264_decode_params *d = &f->decode;
			const struct v4l2_ctrl_h264_sps *s = &f->sps;
			const struct v4l2_ctrl_h264_pps *p = &f->pps;
			sl->first_mb_in_slice = h264_ue(&b);
			uint32_t type = h264_ue(&b), pps_id = h264_ue(&b);
			if (type > 9 || type % 5 != V4L2_H264_SLICE_TYPE_I || pps_id != p->pic_parameter_set_id) goto out;
			sl->slice_type = V4L2_H264_SLICE_TYPE_I;
			d->flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
			d->nal_ref_idc = (stream[start] >> 5) & 3;
			d->frame_num = h264_get(&b, s->log2_max_frame_num_minus4 + 4);
			uint32_t idr_id = h264_ue(&b); if (idr_id > 65535) goto out;
			d->idr_pic_id = idr_id;
			if (!s->pic_order_cnt_type) {
				d->pic_order_cnt_lsb = h264_get(&b, s->log2_max_pic_order_cnt_lsb_minus4 + 4);
				d->top_field_order_cnt = d->pic_order_cnt_lsb;
				if (p->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) d->delta_pic_order_cnt_bottom = h264_se(&b);
				d->bottom_field_order_cnt = d->top_field_order_cnt + d->delta_pic_order_cnt_bottom;
			}
			if (p->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT) {
				uint32_t redundant = h264_ue(&b); if (redundant > 127) goto out;
				sl->redundant_pic_cnt = redundant;
			}
			if (!d->nal_ref_idc) goto out;
			(void)h264_get(&b, 2); d->dec_ref_pic_marking_bit_size = 2;
			int qp = h264_se(&b); if (qp < -51 || qp > 51) goto out;
			sl->slice_qp_delta = qp;
			if (p->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
				uint32_t disable = h264_ue(&b); if (disable > 2) goto out;
				sl->disable_deblocking_filter_idc = disable;
				if (disable != 1) {
					int alpha = h264_se(&b), beta = h264_se(&b);
					if (alpha < -6 || alpha > 6 || beta < -6 || beta > 6) goto out;
					sl->slice_alpha_c0_offset_div2 = alpha; sl->slice_beta_offset_div2 = beta;
				}
			}
			sl->header_bit_size = b.bit;
			f->nal = stream + start; f->nal_size = end - start; f->rbsp_size = size;
		} else if (nal_type != 6 && nal_type != 9 && nal_type != 12) goto out;
		if (b.bad) goto out;
	}
	if (f->nal) result = 0;
out:
	free(rbsp);
	return result;
}
#endif
