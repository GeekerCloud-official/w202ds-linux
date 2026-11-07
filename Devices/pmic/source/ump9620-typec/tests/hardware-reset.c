// SPDX-License-Identifier: GPL-2.0
/* Fault injection against the production probe-only PD block reset. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define W202_UMP9620_TYPEC_H
#define BIT(n) (1U << (n))
#include "../registers.h"
struct w202_typec { int regmap; uint64_t block_reset_count; };
static u32 reset_register;
static int calls, fail_at, delays, release_writes;
static bool write_then_error, stuck;
static int regmap_read(int map, u32 reg, u32 *value)
{
	assert(!map && reg == UMP9620_SOFT_RST1);
	if (++calls == fail_at)
		return -EIO;
	*value = reset_register;
	return 0;
}
static int regmap_update_bits(int map, u32 reg, u32 mask, u32 value)
{
	bool fail = ++calls == fail_at;
	assert(!map && reg == UMP9620_SOFT_RST1 && mask == BIT(2));
	assert(value == 0 || value == BIT(2));
	if (!value)
		release_writes++;
	if ((!fail || write_then_error) && !(stuck && !value))
		reset_register = (reset_register & ~mask) | value;
	return fail ? -EIO : 0;
}
static void usleep_range(unsigned long lo, unsigned long hi)
{
	assert(lo >= 1000 && hi >= lo);
	assert(reset_register & BIT(2));
	delays++;
}
#include "../hardware-reset.c"
static struct w202_typec fresh(void)
{
	reset_register = 0xa; /* Other Type-C/timer reset bits must survive. */
	calls = fail_at = delays = release_writes = 0;
	write_then_error = stuck = false;
	return (struct w202_typec){0};
}
int main(void)
{
	struct w202_typec pd = fresh();
	int i;
	assert(!w202_typec_reset_block(&pd));
	assert(reset_register == 0xa && pd.block_reset_count == 1);
	assert(calls == 4 && delays == 1 && release_writes == 1);
	for (i = 1; i <= 4; i++) {
		pd = fresh(); fail_at = i;
		assert(w202_typec_reset_block(&pd) == -EIO);
		assert(!pd.block_reset_count);
		assert((reset_register & ~BIT(2)) == 0xa);
		if (i == 2)
			assert(release_writes == 1 && !(reset_register & BIT(2)));
	}
	pd = fresh(); fail_at = 2; write_then_error = true;
	assert(w202_typec_reset_block(&pd) == -EIO);
	assert(reset_register == 0xa && release_writes == 1 && !pd.block_reset_count);
	pd = fresh(); reset_register |= BIT(2);
	assert(w202_typec_reset_block(&pd) == -EBUSY && calls == 1);
	assert(!release_writes && !pd.block_reset_count);
	pd = fresh(); stuck = true;
	assert(w202_typec_reset_block(&pd) == -EIO && !pd.block_reset_count);
	puts("PASS: production PD-only reset, shared-bit preservation, read/write failures, lost ACK, existing reset and stuck release");
	return 0;
}
