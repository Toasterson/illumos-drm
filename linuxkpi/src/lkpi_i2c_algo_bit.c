/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's I2C bit-banging algorithm (<linux/i2c-algo-bit.h>): an
 * independent implementation of the I2C master protocol over a bus
 * driver's four line callbacks (struct i2c_algo_bit_data).
 *
 * Both lines are open drain: set*(1) releases a line (it floats high
 * unless a device holds it low), set*(0) drives it low. Between bytes the
 * master leaves SCL low; SDA changes only while SCL is low, except for
 * the start (SDA falls while SCL is high) and stop (SDA rises while SCL
 * is high) conditions. Each clock phase lasts the half period
 * bit->udelay microseconds; SDA is set up half of that before SCL rises.
 * Releasing SCL waits until the line reads high: a slave may hold it low
 * (clock stretching) up to the timeout (bit->timeout jiffies, else
 * adap->timeout), then the transfer fails with -ETIMEDOUT. Without
 * getscl the clock cannot be read and the half period is simply waited.
 *
 * All waits are drv_usecwait(9F) busy-waits (the half periods are a few
 * microseconds); the stretching wait polls until the jiffies deadline.
 * Transfers run in thread context with the bus lock held by the caller.
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/bug.h>
#include <asm/barrier.h>

/* The state of one transfer: the callbacks and the timing. */
struct lkpi_bb {
	const struct i2c_algo_bit_data *bit;
	void *data;
	int half;		/* half clock period, microseconds */
	unsigned long stretch;	/* clock stretching limit, jiffies */
};

static void
lkpi_bb_wait(int us)
{
	if (us > 0)
		udelay(us);
}

static void
lkpi_bb_sda(const struct lkpi_bb *bb, int v)
{
	bb->bit->setsda(bb->data, v);
}

/* SCL low, then the first half of the low phase. */
static void
lkpi_bb_scl_low(const struct lkpi_bb *bb)
{
	bb->bit->setscl(bb->data, 0);
	lkpi_bb_wait(bb->half / 2);
}

/*
 * Releases SCL and waits until it is high (a slave may stretch the
 * clock), then the high phase. 0 or -ETIMEDOUT.
 */
static int
lkpi_bb_scl_high(const struct lkpi_bb *bb)
{
	unsigned long deadline;

	bb->bit->setscl(bb->data, 1);
	if (bb->bit->getscl != NULL) {
		deadline = jiffies + bb->stretch;
		while (bb->bit->getscl(bb->data) == 0) {
			if (time_after(jiffies, deadline)) {
				/* released while the deadline passed? */
				if (bb->bit->getscl(bb->data) != 0)
					break;
				return (-ETIMEDOUT);
			}
			cpu_relax();
		}
	}
	lkpi_bb_wait(bb->half);
	return (0);
}

/* Data setup time before SCL rises: the second half of the low phase. */
static void
lkpi_bb_setup(const struct lkpi_bb *bb)
{
	lkpi_bb_wait((bb->half + 1) / 2);
}

/* Start condition from an idle bus (both lines high). */
static void
lkpi_bb_start(const struct lkpi_bb *bb)
{
	lkpi_bb_sda(bb, 0);
	lkpi_bb_wait(bb->half);
	lkpi_bb_scl_low(bb);
}

/* Repeated start from SCL low. 0 or -ETIMEDOUT. */
static int
lkpi_bb_restart(const struct lkpi_bb *bb)
{
	int ret;

	lkpi_bb_sda(bb, 1);
	lkpi_bb_setup(bb);
	if ((ret = lkpi_bb_scl_high(bb)) != 0)
		return (ret);
	lkpi_bb_sda(bb, 0);
	lkpi_bb_wait(bb->half);
	lkpi_bb_scl_low(bb);
	return (0);
}

/* Stop condition from SCL low; leaves the bus idle. 0 or -ETIMEDOUT. */
static int
lkpi_bb_stop(const struct lkpi_bb *bb)
{
	int ret;

	lkpi_bb_sda(bb, 0);
	lkpi_bb_setup(bb);
	ret = lkpi_bb_scl_high(bb);
	lkpi_bb_sda(bb, 1);
	lkpi_bb_wait(bb->half);
	return (ret);
}

/*
 * Sends a byte, most significant bit first, and reads the receiver's
 * acknowledgement: 1 (ACK: SDA held low), 0 (NACK) or -ETIMEDOUT.
 */
static int
lkpi_bb_write_byte(const struct lkpi_bb *bb, u8 byte)
{
	int i, ret, ack;

	for (i = 7; i >= 0; i--) {
		lkpi_bb_sda(bb, (byte >> i) & 1);
		lkpi_bb_setup(bb);
		if ((ret = lkpi_bb_scl_high(bb)) != 0)
			return (ret);
		lkpi_bb_scl_low(bb);
	}
	lkpi_bb_sda(bb, 1);		/* release SDA for the receiver */
	lkpi_bb_setup(bb);
	if ((ret = lkpi_bb_scl_high(bb)) != 0)
		return (ret);
	ack = bb->bit->getsda(bb->data) == 0;
	lkpi_bb_scl_low(bb);
	return (ack ? 1 : 0);
}

/* Receives a byte (the transmitter drives SDA). 0 or -ETIMEDOUT. */
static int
lkpi_bb_read_byte(const struct lkpi_bb *bb, u8 *byte)
{
	int i, ret;
	u8 v = 0;

	lkpi_bb_sda(bb, 1);
	for (i = 0; i < 8; i++) {
		lkpi_bb_setup(bb);
		if ((ret = lkpi_bb_scl_high(bb)) != 0)
			return (ret);
		v = (u8)((v << 1) | (bb->bit->getsda(bb->data) != 0));
		lkpi_bb_scl_low(bb);
	}
	*byte = v;
	return (0);
}

/* The receiver's acknowledgement bit: ACK (SDA low) or NACK. */
static int
lkpi_bb_send_ack(const struct lkpi_bb *bb, bool ack)
{
	int ret;

	lkpi_bb_sda(bb, ack ? 0 : 1);
	lkpi_bb_setup(bb);
	if ((ret = lkpi_bb_scl_high(bb)) != 0)
		return (ret);
	lkpi_bb_scl_low(bb);
	return (0);
}

/*
 * The address byte of msg, sent again (stop, start) up to retries times
 * while it is not acknowledged. 1 (ACK), 0 (NACK) or -ETIMEDOUT.
 */
static int
lkpi_bb_address(const struct lkpi_bb *bb, const struct i2c_msg *msg,
    int retries)
{
	u8 addr = (u8)(msg->addr << 1);
	int ret, try;

	if ((msg->flags & I2C_M_RD) != 0)
		addr |= 1;
	if ((msg->flags & I2C_M_REV_DIR_ADDR) != 0)
		addr ^= 1;
	for (try = 0; ; try++) {
		ret = lkpi_bb_write_byte(bb, addr);
		if (ret != 0 || try >= retries)
			return (ret);
		if ((ret = lkpi_bb_stop(bb)) != 0)
			return (ret);
		lkpi_bb_wait(bb->half);
		lkpi_bb_start(bb);
	}
}

/* A write message's data. 0, -EIO (NACK) or -ETIMEDOUT. */
static int
lkpi_bb_write_msg(const struct lkpi_bb *bb, const struct i2c_msg *msg)
{
	int i, ret;

	for (i = 0; i < msg->len; i++) {
		ret = lkpi_bb_write_byte(bb, msg->buf[i]);
		if (ret < 0)
			return (ret);
		if (ret == 0 && (msg->flags & I2C_M_IGNORE_NAK) == 0)
			return (-EIO);
	}
	return (0);
}

/*
 * A read message's data: ACK after each byte but the last, NACK after
 * the last, no acknowledgement bit at all with I2C_M_NO_RD_ACK. With
 * I2C_M_RECV_LEN the first byte is the count of the bytes that follow
 * (1..I2C_SMBUS_BLOCK_MAX); it is added to msg->len, as Linux does (the
 * initial len counts the count byte and any PEC byte).
 * 0, -EPROTO (bad count) or -ETIMEDOUT.
 */
static int
lkpi_bb_read_msg(const struct lkpi_bb *bb, struct i2c_msg *msg)
{
	bool ack_bits = (msg->flags & I2C_M_NO_RD_ACK) == 0;
	int i, ret;
	u8 v;

	for (i = 0; i < msg->len; i++) {
		if ((ret = lkpi_bb_read_byte(bb, &v)) != 0)
			return (ret);
		msg->buf[i] = v;
		if (i == 0 && (msg->flags & I2C_M_RECV_LEN) != 0) {
			if (v == 0 || v > I2C_SMBUS_BLOCK_MAX) {
				if (ack_bits)
					(void) lkpi_bb_send_ack(bb, false);
				return (-EPROTO);
			}
			msg->len += v;
		}
		if (ack_bits &&
		    (ret = lkpi_bb_send_ack(bb, i + 1 < msg->len)) != 0)
			return (ret);
	}
	return (0);
}

/* Refuses what the protocol code cannot do before touching the bus. */
static int
lkpi_bb_check(const struct i2c_msg *msgs, int num)
{
	int i;

	if (msgs == NULL || num < 1)
		return (-EINVAL);
	for (i = 0; i < num; i++) {
		if ((msgs[i].flags & I2C_M_TEN) != 0)
			return (-EINVAL);
		if (msgs[i].len > 0 && msgs[i].buf == NULL)
			return (-EINVAL);
		/* the count byte needs a buffer */
		if ((msgs[i].flags & (I2C_M_RD | I2C_M_RECV_LEN)) ==
		    (I2C_M_RD | I2C_M_RECV_LEN) && msgs[i].len == 0)
			return (-EINVAL);
	}
	return (0);
}

static int
lkpi_bit_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	const struct i2c_algo_bit_data *bit;
	struct lkpi_bb bb;
	int i, ret, sret;

	if (WARN(adap == NULL, "i2c-algo-bit: NULL adapter"))
		return (-EINVAL);
	bit = adap->algo_data;
	if (WARN(bit == NULL || bit->setsda == NULL || bit->setscl == NULL ||
	    bit->getsda == NULL, "i2c-algo-bit: %s: no line callbacks",
	    adap->name))
		return (-EINVAL);
	if ((ret = lkpi_bb_check(msgs, num)) != 0)
		return (ret);

	bb.bit = bit;
	bb.data = bit->data;
	bb.half = MAX(bit->udelay, 0);
	if (bit->timeout > 0)
		bb.stretch = (unsigned long)bit->timeout;
	else if (adap->timeout > 0)
		bb.stretch = (unsigned long)adap->timeout;
	else
		bb.stretch = HZ;

	if (bit->pre_xfer != NULL && (ret = bit->pre_xfer(adap)) < 0)
		return (ret);

	lkpi_bb_start(&bb);
	ret = 0;
	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];

		if ((m->flags & I2C_M_NOSTART) == 0) {
			if (i > 0 && (ret = lkpi_bb_restart(&bb)) != 0)
				break;
			ret = lkpi_bb_address(&bb, m, MAX(adap->retries, 0));
			if (ret < 0)
				break;
			if (ret == 0 && (m->flags & I2C_M_IGNORE_NAK) == 0) {
				ret = -ENXIO;
				break;
			}
			ret = 0;
		}
		if ((m->flags & I2C_M_RD) != 0)
			ret = lkpi_bb_read_msg(&bb, m);
		else
			ret = lkpi_bb_write_msg(&bb, m);
		if (ret != 0)
			break;
	}
	sret = lkpi_bb_stop(&bb);
	if (ret == 0 && sret != 0)
		ret = sret;

	if (bit->post_xfer != NULL)
		bit->post_xfer(adap);
	return (ret < 0 ? ret : num);
}

static u32
lkpi_bit_func(struct i2c_adapter *adap)
{
	(void) adap;
	return (I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_EMUL |
	    I2C_FUNC_PROTOCOL_MANGLING);
}

const struct i2c_algorithm i2c_bit_algo = {
	.master_xfer = lkpi_bit_xfer,
	.functionality = lkpi_bit_func
};

static int
lkpi_bit_prepare(struct i2c_adapter *adap)
{
	if (WARN(adap == NULL, "i2c-algo-bit: NULL adapter"))
		return (-EINVAL);
	adap->algo = &i2c_bit_algo;
	if (adap->retries == 0)
		adap->retries = 3;
	return (0);
}

int
i2c_bit_add_bus(struct i2c_adapter *adap)
{
	int ret;

	if ((ret = lkpi_bit_prepare(adap)) != 0)
		return (ret);
	return (i2c_add_adapter(adap));
}

int
i2c_bit_add_numbered_bus(struct i2c_adapter *adap)
{
	int ret;

	if ((ret = lkpi_bit_prepare(adap)) != 0)
		return (ret);
	return (i2c_add_numbered_adapter(adap));
}
