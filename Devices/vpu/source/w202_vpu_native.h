/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_VPU_NATIVE_H
#define W202_VPU_NATIVE_H

#include <linux/atomic.h>
#include <linux/device.h>
#include "w202_vpu_buffer_uapi.h"
#include "w202_vpu_h264.h"

struct codec_file;
struct dma_buf;
struct w202_vpu_video;

int w202_vpu_native_begin(struct device *dev, struct codec_file **context);
int w202_vpu_native_import(struct codec_file *context, struct dma_buf *buf,
			   struct w202_vpu_iova *map);
int w202_vpu_native_validate_buffers(struct codec_file *context,
				     const struct w202_vpu_iova *input,
				     const struct w202_vpu_iova *frame);
int w202_vpu_native_execute(struct codec_file *context,
			    const struct w202_h264_config *config,
			    const atomic_t *abort);
/* Reset precedes all unmapping. Failed reset quarantines context and imports. */
int w202_vpu_native_end(struct codec_file *context);
void w202_vpu_native_wake(struct device *dev);

struct w202_vpu_video *w202_vpu_video_register(struct device *dev);
void w202_vpu_video_unregister(struct w202_vpu_video *video);

#endif
