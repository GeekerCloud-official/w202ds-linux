/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "w202_hub_protocol.h"

struct capture { unsigned int count; u8 last[W202_HUB_PAYLOAD_MAX]; unsigned int length; };

static void capture(void *context, u8 handle, u8 subtype,
		    const u8 *bytes, unsigned int length)
{
	struct capture *c = context;
	(void)handle;
	(void)subtype;
	c->count++;
	c->length = length;
	memcpy(c->last, bytes, length);
}

int main(void)
{
	const u8 version_command[] = {0x7e,0x7e,0x7e,0x7e,0xff,0xcd,0,1,3,0x34,0xff,0,0xff};
	const u8 info_command[] = {0x7e,0x7e,0x7e,0x7e,0xff,9,0,1,3,0xf8,0xff,0,0xff};
	const u8 version_response[] = {0x7e,0x7e,0x7e,0x7e,0xff,0xcd,0,4,3,0x31,0xed,0xff,0x34,1,0xdd,0xfe};
	u8 frame[W202_HUB_FRAME_MAX], payload[W202_HUB_PAYLOAD_MAX], ff = 255;
	struct hub_parser *p = calloc(2, sizeof(*p));
	struct capture c = {0};
	s64 fixed;
	int n;
	unsigned int i;
	assert(p);
	n = hub_encode(frame, sizeof(frame), 255, 0xcd, &ff, 1);
	assert(n == sizeof(version_command) && !memcmp(frame, version_command, n));
	n = hub_encode(frame, sizeof(frame), 255, 9, &ff, 1);
	assert(n == sizeof(info_command) && !memcmp(frame, info_command, n));
	hub_feed(p, (const u8 *)"junk~~", 6, capture, &c);
	for (i = 0; i < sizeof(version_response); i++)
		hub_feed(p, version_response + i, 1, capture, &c);
	assert(c.count == 1 && c.length == 4 && c.last[0] == 0xed);
	memset(payload, 0x7e, sizeof(payload));
	n = hub_encode(frame, sizeof(frame), 14, 4, payload, sizeof(payload));
	assert(n == sizeof(frame));
	hub_feed(p, frame, 13, capture, &c);
	hub_feed(p + 1, version_response, sizeof(version_response), capture, &c);
	hub_feed(p, frame + 13, n - 13, capture, &c);
	assert(c.count == 3 && c.length == sizeof(payload) && !memcmp(c.last, payload, sizeof(payload)));
	frame[n - 1] ^= 1;
	hub_feed(p, frame, n, capture, &c);
	assert(p->bad_payloads == 1 && c.count == 3);
	frame[8] ^= 1;
	hub_feed(p, frame, n, capture, &c);
	hub_feed(p, version_response, sizeof(version_response), capture, &c);
	assert(c.count == 4);
	n = hub_encode(frame, sizeof(frame), 14, 0xd1, NULL, 0);
	assert(n == 10);
	hub_feed(p, frame, n, capture, &c);
	hub_feed(p, frame, n, capture, &c);
	assert(c.count == 6 && c.length == 0);
	assert(hub_encode(frame, sizeof(frame), 1, 4, payload, sizeof(payload) + 1) == -EMSGSIZE);
	assert(hub_encode(frame, 12, 1, 4, &ff, 1) == -EMSGSIZE);
	assert(!hub_float_fixed(0x3fa00000, 1000000, &fixed) && fixed == 1250000);
	assert(!hub_float_fixed(0xc0200000, 1000000, &fixed) && fixed == -2500000);
	assert(!hub_float_fixed(0x3fa00000, 10000, &fixed) && fixed == 12500);
	assert(!hub_float_fixed(0x80000000, 1000000, &fixed) && fixed == 0);
	assert(!hub_float_fixed(0x00000001, 1000000, &fixed) && fixed == 0);
	assert(hub_float_fixed(0x7f800000, 1000000, &fixed) == -ERANGE);
	assert(hub_float_fixed(0x7fc00001, 1000000, &fixed) == -ERANGE);
	assert(hub_float_fixed(0x7f7fffff, 1000000, &fixed) == -ERANGE);
	free(p);
	puts("PASS: live wire vectors, fragmentation, independent rings, maximum frame, CRC recovery, zero payload, fixed point and non-finite/overflow rejection");
	return 0;
}
