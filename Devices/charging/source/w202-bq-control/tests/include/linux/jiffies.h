/* SPDX-License-Identifier: GPL-2.0-only */
extern unsigned long jiffies;
#define msecs_to_jiffies(ms) (ms)
#define time_before(a, b) ((long)((a) - (b)) < 0)
