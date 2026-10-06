/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's I2C bit-banging algorithm (include/linux/i2c-algo-bit.h,
 * GPL-2.0; independent declarations of the same interface;
 * src/lkpi_i2c_algo_bit.c, an independent implementation of the I2C
 * protocol over the four line callbacks).
 *
 * The bus driver fills a struct i2c_algo_bit_data with its line
 * callbacks (setsda/setscl drive a line low (0) or release it (1);
 * getsda/getscl read it; getscl may be NULL for a bus whose clock cannot
 * be read, then no clock stretching is detected), the half clock period
 * udelay (microseconds) and the clock-stretching timeout (jiffies), points
 * adap->algo_data at it and registers the adapter with i2c_bit_add_bus().
 * pre_xfer/post_xfer (optional) run around every transfer (pre_xfer's
 * negative errno ends it).
 *
 * i2c_bit_algo is the algorithm itself: a driver may call its
 * master_xfer directly on an adapter whose algo_data is such a structure
 * (i915 falls back to bit banging this way). Its transfers send a start
 * condition, each message's address byte (7-bit; 10-bit addresses are
 * refused with -EINVAL) and data with acknowledgements, repeated starts
 * between messages (none for I2C_M_NOSTART), and a stop condition at the
 * end; a missing acknowledgement ends the transfer with -ENXIO (address)
 * or -EIO (data) unless I2C_M_IGNORE_NAK; a clock held low longer than
 * the timeout ends it with -ETIMEDOUT. The functionality is I2C_FUNC_I2C,
 * I2C_FUNC_NOSTART, I2C_FUNC_SMBUS_EMUL and I2C_FUNC_PROTOCOL_MANGLING.
 *
 * Details (as Linux): a NACKed address byte is sent again up to
 * adap->retries times (stop, start, address) before -ENXIO; the last
 * byte of a read message is NACKed, the others ACKed, and
 * I2C_M_NO_RD_ACK skips the acknowledgement bit; I2C_M_RECV_LEN takes the
 * first byte read as the count of the bytes that follow (1 to
 * I2C_SMBUS_BLOCK_MAX, else -EPROTO) and adds it to msg->len (the buffer
 * must have room for it); I2C_M_REV_DIR_ADDR inverts the address byte's
 * read bit; I2C_M_NOSTART omits the (repeated) start and the address
 * byte, also for the first message. The clock-stretching limit is
 * timeout, or adap->timeout if timeout is not positive. A transfer runs
 * in thread context and busy-waits (drv_usecwait(9F)) for its timing;
 * the caller holds the bus lock (i2c_transfer()). A missing algo_data or
 * setsda/setscl/getsda callback, a NULL or empty message array and a
 * message without buffer are refused with -EINVAL.
 *
 * i2c_bit_add_bus(adap) / i2c_bit_add_numbered_bus(adap)
 *	set adap->algo to i2c_bit_algo, adap->retries to 3 if it is 0, and
 *	register the adapter (i2c_add_adapter() /
 *	i2c_add_numbered_adapter()). The bus is not tested first.
 */
#ifndef _LINUX_I2C_ALGO_BIT_H
#define	_LINUX_I2C_ALGO_BIT_H

#include <linux/i2c.h>

struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *data, int state);
	void (*setscl)(void *data, int state);
	int (*getsda)(void *data);
	int (*getscl)(void *data);
	int (*pre_xfer)(struct i2c_adapter *adap);
	void (*post_xfer)(struct i2c_adapter *adap);
	int udelay;		/* half clock period, microseconds */
	int timeout;		/* clock stretching limit, jiffies */
	bool can_do_atomic;
};

extern const struct i2c_algorithm i2c_bit_algo;
extern int i2c_bit_add_bus(struct i2c_adapter *adap);
extern int i2c_bit_add_numbered_bus(struct i2c_adapter *adap);

#endif
