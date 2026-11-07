// SPDX-License-Identifier: GPL-2.0-only
/* Verified chip writes; no reset or power-path/GPIO manipulation. */
#include "charger.h"

int w202_bq_stop_locked(struct w202_bq *bq)
{
	int value, verify, ret;

	value = i2c_smbus_read_byte_data(bq->client, 1);
	if (value < 0)
		return value;
	if (value & CHARGE_ENABLE) {
		ret = i2c_smbus_write_byte_data(bq->client, 1,
					      value & ~(CHARGE_ENABLE | WATCHDOG_STROBE));
		if (ret)
			return ret;
	}
	verify = i2c_smbus_read_byte_data(bq->client, 1);
	if (verify < 0)
		return verify;
	return verify & CHARGE_ENABLE ? -EIO : 0;
}

int w202_bq_update_locked(struct w202_bq *bq, int reg, int mask, int bits)
{
	int value = i2c_smbus_read_byte_data(bq->client, reg);
	int ret, wanted;

	if (value < 0)
		return value;
	wanted = (value & ~mask) | bits;
	if (wanted != value) {
		ret = i2c_smbus_write_byte_data(bq->client, reg, wanted);
		if (ret)
			return ret;
	}
	ret = i2c_smbus_read_byte_data(bq->client, reg);
	return ret < 0 ? ret : (ret & mask) != bits ? -EIO : 0;
}

/* Stock sqc_bq2560x REG06[7:6]: 6.5 / 10.5 / 14 V input OVP.
 * Keep boost voltage and VINDPM intact. Prepare before asking the source
 * to raise VBUS, on both input-connected chips, with battery charging off.
 */
int w202_bq_prepare_input_locked(struct w202_bq *bq)
{
	int bits = w202_bq_ovp_bits(bq);
	int value = i2c_smbus_read_byte_data(bq->client, 6);
	int ret;

	if (value < 0)
		return value;
	if ((value & 0xc0) == bits)
		return 0;
	ret = w202_bq_stop_locked(bq);
	return ret ? ret : w202_bq_update_locked(bq, 6, 0xc0, bits);
}
