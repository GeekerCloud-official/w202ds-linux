/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_VPU_H264_H
#define W202_VPU_H264_H

#include <linux/types.h>
#include <linux/v4l2-controls.h>

/* Internal, pointer-free hardware result. Not a userspace ABI. Building this
 * result does not access hardware and can be checked against observed jobs.
 * The caller owns every DMA mapping until the core is stopped/reset.
 */
#define W202_H264_MAX_REGS 1024
#define W202_H264_BSM_PADDING 1024
struct w202_h264_reg {
	__u32 offset;
	__u32 value;
};
struct w202_h264_config {
	__u32 width, height;
	__u32 stream_address, stream_bytes, header_bits;
	unsigned int count;
	struct w202_h264_reg regs[W202_H264_MAX_REGS];
};
#define W202_H264_MAX_REFERENCES 16
struct w202_h264_reference {
	__u64 address, auxiliary_address;
	__u64 capacity, auxiliary_capacity;
	__u8 frame_id;
};
struct w202_h264_job {
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_slice_params *slice;
	const struct v4l2_ctrl_h264_decode_params *decode;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling;
	const struct v4l2_ctrl_h264_pred_weights *weights;
	__u64 input_address, output_address, auxiliary_address;
	__u64 input_capacity, output_capacity, auxiliary_capacity;
	__u32 input_bytes;
	/* NAL size after removal of emulation-prevention bytes, including the
	 * NAL header; OUTPUT input_bytes still describes the original EBSP. */
	__u32 nalu_bytes;
	/* Legacy diagnostics: address/metadata for one active L0 entry.
	 * Its position in slice.ref_pic_list0 selects decode.dpb; frame_id is the
	 * stable identity stored in motion-vector reference metadata (0..31).
	 */
	__u64 reference_address, reference_auxiliary_address;
	__u64 reference_capacity, reference_auxiliary_capacity;
	__u8 reference_frame_id;
	/* Legacy diagnostics: the first L1 entry, independently mapped. */
	__u64 reference1_address, reference1_auxiliary_address;
	__u64 reference1_capacity, reference1_auxiliary_capacity;
	__u8 reference1_frame_id;
	/* Optional arrays of up to MAX_REFERENCES entries, in slice-list order.
	 * Legacy fields above describe one entry per list when arrays are NULL. */
	const struct w202_h264_reference *reference_lists[2];
};

/* First validated subset: progressive Baseline, one CAVLC IDR slice, NV12.
 * Unsupported syntax is rejected before any DMA or register programming. */
int w202_h264_build_idr(const struct w202_h264_job *job,
		       struct w202_h264_config *config);
/* Progressive 8-bit 4:2:0 Baseline/Main/High IDR/P, one slice and up to sixteen
 * short-term references. High supports 8x8 transforms and resolved scaling.
 * Shared by the experimental V4L2 frontend and the native diagnostic client.
 */
int w202_h264_build_ip(const struct w202_h264_job *job,
		      struct w202_h264_config *config);
/* Also accepts progressive Main/High reference/non-reference B pictures,
 * spatial/temporal direct and explicit/implicit weights. Up to sixteen entries
 * per list; hardware qualification and remaining limits are in v4l2.md. */
int w202_h264_build_ipb(const struct w202_h264_job *job,
		       struct w202_h264_config *config);

#endif
