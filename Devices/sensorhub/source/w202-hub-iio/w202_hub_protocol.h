/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_HUB_PROTOCOL_H
#define W202_HUB_PROTOCOL_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#endif

#define W202_HUB_PAYLOAD_MAX 2036
#define W202_HUB_FRAME_MAX (W202_HUB_PAYLOAD_MAX + 12)

static inline u16 hub_be16(const u8 *p)
{
	return (u16)p[0] << 8 | p[1];
}

static inline u16 hub_checksum(const u8 *p, unsigned int length)
{
	u32 sum = 0;

	while (length >= 2) {
		sum += hub_be16(p);
		p += 2;
		length -= 2;
	}
	if (length)
		sum += (u16)*p << 8;
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

static inline int hub_encode(u8 *frame, unsigned int capacity, u8 handle,
			     u8 subtype, const u8 *payload, unsigned int length)
{
	u16 crc;
	unsigned int total = 10 + (length ? length + 2 : 0);

	if (length > W202_HUB_PAYLOAD_MAX || total > capacity || (!payload && length))
		return -EMSGSIZE;
	memset(frame, 0x7e, 4);
	frame[4] = handle;
	frame[5] = subtype;
	frame[6] = length >> 8;
	frame[7] = length;
	crc = hub_checksum(frame, 8);
	frame[8] = crc >> 8;
	frame[9] = crc;
	if (length) {
		memcpy(frame + 10, payload, length);
		crc = hub_checksum(payload, length);
		frame[10 + length] = crc >> 8;
		frame[11 + length] = crc;
	}
	return total;
}

struct hub_parser {
	u8 bytes[W202_HUB_FRAME_MAX];
	unsigned int used;
	u64 frames;
	u64 bad_headers;
	u64 bad_payloads;
};

typedef void (*hub_packet_fn)(void *context, u8 handle, u8 subtype,
			      const u8 *payload, unsigned int length);

/* Separate state is required for each receive ring. Callback data is temporary. */
static inline void hub_feed(struct hub_parser *p, const u8 *bytes,
			    unsigned int length, hub_packet_fn callback, void *context)
{
	unsigned int n, payload_length, total;

	for (n = 0; n < length; n++) {
		p->bytes[p->used++] = bytes[n];
		while (p->used >= 10) {
			payload_length = hub_be16(p->bytes + 6);
			if (memcmp(p->bytes, "~~~~", 4) ||
			    payload_length > W202_HUB_PAYLOAD_MAX ||
			    hub_checksum(p->bytes, 8) != hub_be16(p->bytes + 8)) {
				p->bad_headers++;
				memmove(p->bytes, p->bytes + 1, --p->used);
				continue;
			}
			total = 10 + (payload_length ? payload_length + 2 : 0);
			if (p->used < total)
				break;
			if (payload_length && hub_checksum(p->bytes + 10, payload_length) !=
			    hub_be16(p->bytes + 10 + payload_length)) {
				p->bad_payloads++;
			} else {
				p->frames++;
				callback(context, p->bytes[4], p->bytes[5],
					 p->bytes + 10, payload_length);
			}
			p->used -= total;
			memmove(p->bytes, p->bytes + total, p->used);
		}
	}
}

/* IEEE754 binary32 to signed fixed point, without kernel floating point.
 * Non-finite/overflowing input is rejected. Small finite values round toward 0.
 */
static inline int hub_float_fixed(u32 bits, u32 factor, s64 *value)
{
	u32 exponent = (bits >> 23) & 255;
	u64 magnitude;
	int shift;

	if (exponent == 255)
		return -ERANGE;
	if (!exponent) {
		*value = 0;
		return 0;
	}
	magnitude = (u64)((bits & 0x7fffff) | 0x800000) * factor;
	shift = (int)exponent - 150;
	if (shift >= 0) {
		if (shift >= 63 || magnitude > ((~(u64)0 >> 1) >> shift))
			return -ERANGE;
		magnitude <<= shift;
	} else if (shift <= -64) {
		magnitude = 0;
	} else {
		magnitude >>= -shift;
	}
	*value = (bits >> 31) ? -(s64)magnitude : (s64)magnitude;
	return 0;
}
#endif
