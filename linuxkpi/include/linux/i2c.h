/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's I2C core for bus drivers (include/linux/i2c.h and
 * include/uapi/linux/i2c.h, GPL-2.0; independent declarations of the same
 * interface: names, constants and the adapter/message structures with the
 * fields DRM code uses; src/lkpi_i2c.c).
 *
 * Only adapters exist (no I2C clients, drivers or device tree): a display
 * driver registers the adapters of its DDC/AUX buses and transfers
 * messages through them (EDID, DP dual mode, SCDC, SDVO).
 *
 * i2c_add_adapter(adap)
 *	gives the adapter the lowest free bus number (adap->nr), names its
 *	device "i2c-<nr>", sets a default timeout of one second (HZ jiffies)
 *	if adap->timeout is 0, initialises the bus lock, points
 *	adap->lock_ops at the default lock operations (over bus_lock) if it
 *	is NULL and lists it for i2c_get_adapter(). The adapter's algorithm
 *	must be set (else -EINVAL); an adapter already listed is refused
 *	with -EBUSY. 0 or a negative errno. Thread context.
 *	The adapter's struct device is initialised as a layer device
 *	(lkpi_device_init(): kobject, devres, reference count) on the
 *	dev_info_t of dev.parent, so that dev_err() on the adapter reports
 *	the parent's driver instance; dev.parent may be NULL (messages then
 *	go to the console without an instance). Driver data set before
 *	registration (i2c_set_adapdata()) is kept. No device node, no sysfs.
 * i2c_add_numbered_adapter(adap)
 *	as i2c_add_adapter() with the bus number adap->nr (-1: any free
 *	number); -EBUSY if it is taken, -EINVAL if it is below -1.
 * i2c_del_adapter(adap)
 *	removes the adapter from the list; waits (sleeping) until no
 *	i2c_get_adapter() reference is left and the bus lock is free, then
 *	releases the device's managed resources. An adapter that is not
 *	listed is ignored. The adapter may be registered again afterwards.
 * i2c_get_adapter(nr) / i2c_put_adapter(adap)
 *	a listed adapter by number, with a reference (NULL if none).
 * i2c_transfer(adap, msgs, num)
 *	locks the bus (adap->lock_ops->lock_bus, else the adapter's own bus
 *	lock), then calls __i2c_transfer(), then unlocks. Thread context.
 * __i2c_transfer(adap, msgs, num)
 *	with the bus locked: calls the algorithm's xfer, again on -EAGAIN up
 *	to adap->retries times while adap->timeout has not passed. Returns
 *	the number of messages transferred or a negative errno; -EOPNOTSUPP
 *	for an algorithm without xfer, -EINVAL for a message without buffer
 *	(len > 0) or num <= 0.
 * i2c_lock_bus / i2c_trylock_bus / i2c_unlock_bus
 *	the adapter's lock operations (flags: I2C_LOCK_ROOT_ADAPTER,
 *	I2C_LOCK_SEGMENT; one level only, both lock the adapter); the
 *	adapter's own bus lock while adap->lock_ops is NULL (an adapter not
 *	registered yet).
 * i2c_get_functionality / i2c_check_functionality
 *	0 (nothing supported) for an adapter without algorithm or without
 *	a functionality callback.
 *
 * Adapter quirks are checked only for the plain limits (quirks->flags 0:
 * max_num_msgs, max_read_len, max_write_len; -EOPNOTSUPP); the combined
 * message flags of Linux (I2C_AQ_*) are not declared and not checked.
 */
#ifndef _LINUX_I2C_H
#define	_LINUX_I2C_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/mod_devicetable.h>

/* struct i2c_msg flags (the uAPI values) */
#define	I2C_M_RD		0x0001
#define	I2C_M_TEN		0x0010
#define	I2C_M_DMA_SAFE		0x0200
#define	I2C_M_RECV_LEN		0x0400
#define	I2C_M_NO_RD_ACK		0x0800
#define	I2C_M_IGNORE_NAK	0x1000
#define	I2C_M_REV_DIR_ADDR	0x2000
#define	I2C_M_NOSTART		0x4000
#define	I2C_M_STOP		0x8000

/* adapter functionality (the uAPI values) */
#define	I2C_FUNC_I2C			0x00000001
#define	I2C_FUNC_10BIT_ADDR		0x00000002
#define	I2C_FUNC_PROTOCOL_MANGLING	0x00000004
#define	I2C_FUNC_SMBUS_PEC		0x00000008
#define	I2C_FUNC_NOSTART		0x00000010
#define	I2C_FUNC_SLAVE			0x00000020
#define	I2C_FUNC_SMBUS_BLOCK_PROC_CALL	0x00008000
#define	I2C_FUNC_SMBUS_QUICK		0x00010000
#define	I2C_FUNC_SMBUS_READ_BYTE	0x00020000
#define	I2C_FUNC_SMBUS_WRITE_BYTE	0x00040000
#define	I2C_FUNC_SMBUS_READ_BYTE_DATA	0x00080000
#define	I2C_FUNC_SMBUS_WRITE_BYTE_DATA	0x00100000
#define	I2C_FUNC_SMBUS_READ_WORD_DATA	0x00200000
#define	I2C_FUNC_SMBUS_WRITE_WORD_DATA	0x00400000
#define	I2C_FUNC_SMBUS_PROC_CALL	0x00800000
#define	I2C_FUNC_SMBUS_READ_BLOCK_DATA	0x01000000
#define	I2C_FUNC_SMBUS_WRITE_BLOCK_DATA	0x02000000
#define	I2C_FUNC_SMBUS_READ_I2C_BLOCK	0x04000000
#define	I2C_FUNC_SMBUS_WRITE_I2C_BLOCK	0x08000000
#define	I2C_FUNC_SMBUS_HOST_NOTIFY	0x10000000
#define	I2C_FUNC_SMBUS_BYTE \
	(I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define	I2C_FUNC_SMBUS_BYTE_DATA \
	(I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define	I2C_FUNC_SMBUS_WORD_DATA \
	(I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define	I2C_FUNC_SMBUS_BLOCK_DATA \
	(I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define	I2C_FUNC_SMBUS_I2C_BLOCK \
	(I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)
#define	I2C_FUNC_SMBUS_EMUL \
	(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE | \
	I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | \
	I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | \
	I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)
#define	I2C_FUNC_SMBUS_EMUL_ALL \
	(I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA | \
	I2C_FUNC_SMBUS_BLOCK_PROC_CALL)

/* adapter classes */
#define	I2C_CLASS_HWMON		(1 << 0)
#define	I2C_CLASS_DDC		(1 << 3)
#define	I2C_CLASS_SPD		(1 << 7)
#define	I2C_CLASS_DEPRECATED	(1 << 8)

#define	I2C_NAME_SIZE		20
#define	I2C_CLIENT_END		0xfffeU

/* i2c_lock_bus() flags */
#define	I2C_LOCK_ROOT_ADAPTER	BIT(0)
#define	I2C_LOCK_SEGMENT	BIT(1)

struct i2c_msg {
	__u16 addr;
	__u16 flags;
	__u16 len;
	__u8 *buf;
};

#define	I2C_SMBUS_BLOCK_MAX	32
union i2c_smbus_data {
	__u8 byte;
	__u16 word;
	__u8 block[I2C_SMBUS_BLOCK_MAX + 2];
};

struct i2c_adapter;
struct i2c_client;

/*
 * The bus algorithm: xfer (also called master_xfer) transfers num
 * messages and returns how many were transferred or a negative errno;
 * functionality returns the I2C_FUNC_* bits. smbus_xfer and the atomic
 * variants are accepted but never called (no SMBus emulation).
 */
struct i2c_algorithm {
	union {
		int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
		    int num);
		int (*master_xfer)(struct i2c_adapter *adap,
		    struct i2c_msg *msgs, int num);
	};
	union {
		int (*xfer_atomic)(struct i2c_adapter *adap,
		    struct i2c_msg *msgs, int num);
		int (*master_xfer_atomic)(struct i2c_adapter *adap,
		    struct i2c_msg *msgs, int num);
	};
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr,
	    unsigned short flags, char read_write, u8 command, int size,
	    union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, u16 addr,
	    unsigned short flags, char read_write, u8 command, int size,
	    union i2c_smbus_data *data);
	u32 (*functionality)(struct i2c_adapter *adap);
};

struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adap, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adap, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adap, unsigned int flags);
};

struct i2c_adapter_quirks {
	u64 flags;
	int max_num_msgs;
	u16 max_write_len;
	u16 max_read_len;
	u16 max_comb_1st_msg_len;
	u16 max_comb_2nd_msg_len;
};

struct i2c_adapter {
	struct lx_module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	const struct i2c_lock_operations *lock_ops;
	struct mutex bus_lock;		/* the default lock operations' */
	int timeout;			/* jiffies; HZ if 0 at registration */
	int retries;
	struct device dev;
	int nr;
	char name[48];
	const struct i2c_adapter_quirks *quirks;
	/* illumos: the I2C core's (src/lkpi_i2c.c) */
	struct list_head lkpi_link;
	unsigned int lkpi_refs;		/* i2c_get_adapter() references */
	bool lkpi_listed;
};

#define	to_i2c_adapter(d)	container_of(d, struct i2c_adapter, dev)

extern int i2c_add_adapter(struct i2c_adapter *adap);
extern int i2c_add_numbered_adapter(struct i2c_adapter *adap);
extern void i2c_del_adapter(struct i2c_adapter *adap);
extern struct i2c_adapter *i2c_get_adapter(int nr);
extern void i2c_put_adapter(struct i2c_adapter *adap);
extern int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
    int num);
extern int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
    int num);

static inline void
i2c_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops != NULL)
		adap->lock_ops->lock_bus(adap, flags);
	else
		mutex_lock(&adap->bus_lock);
}

static inline int
i2c_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops != NULL)
		return (adap->lock_ops->trylock_bus(adap, flags));
	return (mutex_trylock(&adap->bus_lock));
}

static inline void
i2c_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops != NULL)
		adap->lock_ops->unlock_bus(adap, flags);
	else
		mutex_unlock(&adap->bus_lock);
}

static inline void *
i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return (dev_get_drvdata(&adap->dev));
}

static inline void
i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	dev_set_drvdata(&adap->dev, data);
}

static inline u32
i2c_get_functionality(struct i2c_adapter *adap)
{
	if (adap->algo == NULL || adap->algo->functionality == NULL)
		return (0);
	return (adap->algo->functionality(adap));
}

static inline int
i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return ((func & i2c_get_functionality(adap)) == func);
}

static inline u8
i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return ((msg->addr << 1) | (msg->flags & I2C_M_RD));
}

static inline int
i2c_adapter_id(struct i2c_adapter *adap)
{
	return (adap->nr);
}

#endif
