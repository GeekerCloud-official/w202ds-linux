/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_ZSK_PROTOCOL_H
#define W202_ZSK_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stddef.h>
#include <string.h>
#endif

/* Observed keyboard frames: type, modifiers, XOR, six HID usages.
 * Keyboard XOR is zero. Touchpad reports are 21 bytes without an XOR byte.
 */
#define ZSK_FRAME_MAX 21
struct zsk_parser {
	unsigned char data[ZSK_FRAME_MAX];
	unsigned int used;
	unsigned long frames;
	unsigned long bad_checksums;
	unsigned long bad_pad_reports;
	unsigned long discarded;
};

typedef void (*zsk_frame_callback)(void *ctx, const unsigned char *frame,
				 unsigned int length);

static inline int zsk_pad_valid(const unsigned char *frame)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		unsigned int offset = 1 + i * 4;
		unsigned int x = frame[offset + 1] | ((frame[offset + 2] & 15) << 8);
		unsigned int y = (frame[offset + 3] << 4) | (frame[offset + 2] >> 4);

		if ((frame[offset] & ~3u) != i * 4)
			return 0;
		if ((frame[offset] & 2) && (x > 1177 || y > 591))
			return 0;
	}
	return frame[19] <= 4 && frame[20] <= 1;
}

static inline void zsk_parser_shift(struct zsk_parser *p, unsigned int count)
{
	p->used -= count;
	memmove(p->data, p->data + count, p->used);
}

static inline void zsk_parser_feed(struct zsk_parser *p, unsigned char byte,
				   zsk_frame_callback callback, void *ctx)
{
	p->data[p->used++] = byte;
	while (p->used) {
		unsigned int length, i;
		unsigned char checksum = 0;

		if (p->data[0] == 1)
			length = 9;
		else if (p->data[0] == 7)
			length = 21;
		else {
			p->discarded++;
			zsk_parser_shift(p, 1);
			continue;
		}
		if (p->used < length)
			return;
		if (p->data[0] == 1) {
			for (i = 0; i < length; i++)
				checksum ^= p->data[i];
		} else if (!zsk_pad_valid(p->data)) {
			p->bad_pad_reports++;
			p->discarded++;
			zsk_parser_shift(p, 1);
			continue;
		}
		if (checksum) {
			p->bad_checksums++;
			p->discarded++;
			zsk_parser_shift(p, 1);
			continue;
		}
		p->frames++;
		callback(ctx, p->data, length);
		zsk_parser_shift(p, length);
	}
}
#endif
