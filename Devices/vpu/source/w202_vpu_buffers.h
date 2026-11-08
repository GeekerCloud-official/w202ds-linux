/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_VPU_BUFFERS_H
#define W202_VPU_BUFFERS_H
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "w202_vpu_buffer_uapi.h"

struct w202_vpu_buffers {
	struct device *dev;
	struct mutex lock;
	struct list_head maps;
	unsigned int count;
};
struct dma_buf;

void w202_vpu_buffers_init(struct w202_vpu_buffers *ctx, struct device *dev);
void w202_vpu_buffers_destroy(struct w202_vpu_buffers *ctx);
int w202_vpu_buffers_map(struct w202_vpu_buffers *ctx, struct w202_vpu_iova *map);
/* Retains its own reference and attachment until unmap/destroy. */
int w202_vpu_buffers_import(struct w202_vpu_buffers *ctx, struct dma_buf *buf,
			    struct w202_vpu_iova *map);
/* Native jobs import exactly one input/output plus zero to two references.
 * Compare backing SG physical ranges; separate exports can alias storage. */
int w202_vpu_buffers_validate_native(struct w202_vpu_buffers *ctx,
				     const struct w202_vpu_iova *input,
				     const struct w202_vpu_iova *frame);
int w202_vpu_buffers_unmap(struct w202_vpu_buffers *ctx, const struct w202_vpu_iova *map);
int w202_vpu_buffers_verify(struct w202_vpu_buffers *ctx, const struct w202_vpu_iova *map,
			    unsigned int *pages, unsigned int *high_pages);
#endif
