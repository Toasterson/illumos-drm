/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: the I2C core and the bit-banging algorithm against a
 * simulated bus. The line callbacks model two open-drain lines shared by
 * the master (the algorithm) and an EEPROM-like slave at address 0x50
 * (256 bytes; a write's first data byte sets the address pointer, reads
 * and further writes go on from it). The slave reacts to the line edges
 * the master makes: start and stop conditions, bits sampled when SCL
 * rises, its own SDA changed after SCL falls. It can stretch the clock
 * for a number of SCL reads, or hold it low for good.
 *
 * Checked: registration (bus numbers, names, references, -EBUSY,
 * argument validation), a write/read round trip with a repeated start,
 * I2C_M_RECV_LEN, a NACKed address (-ENXIO after the address retries),
 * I2C_M_IGNORE_NAK, a stretched clock, a clock held low (-ETIMEDOUT),
 * pre_xfer/post_xfer, and i2c_del_adapter() waiting for a reference.
 * Byte comparisons use bcmp(9F): memcmp is not a DDI symbol.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>

#include "st.h"

#define	ST_EE_SLAVE	0x50

enum st_ee_state { ST_EE_IDLE, ST_EE_ADDR, ST_EE_WRITE, ST_EE_READ };

struct st_ee {
	int m_sda, m_scl;		/* the master's drive (1: released) */
	int s_sda;			/* the slave's SDA drive */
	bool scl_stuck;			/* slave holds SCL low for good */
	int stretch_reads;		/* SCL reads low after each release */
	int stretch_left;
	enum st_ee_state state;
	int bit;			/* bits received / index of bit sent */
	bool in_ack;			/* slave drives its ACK bit */
	bool rd, ptr_set, master_ack;
	u8 shift, ptr;
	u8 mem[256];
	int starts, stops, addr_nacks, pre, post;
};

struct st_i2c {
	struct i2c_adapter adap;
	struct i2c_algo_bit_data bit;
	struct st_ee ee;
};

static int
st_ee_sda_line(const struct st_ee *ee)
{
	return (ee->m_sda && ee->s_sda);
}

/* SCL as the slave sees it (a short stretch is not an edge of its own). */
static int
st_ee_scl_line(const struct st_ee *ee)
{
	return (ee->m_scl && !ee->scl_stuck);
}

static void
st_ee_send_bit(struct st_ee *ee)
{
	ee->s_sda = (ee->mem[ee->ptr] >> (7 - ee->bit)) & 1;
}

/* SCL rises: sample the data bit, or the master's acknowledgement. */
static void
st_ee_rise(struct st_ee *ee)
{
	switch (ee->state) {
	case ST_EE_ADDR:
	case ST_EE_WRITE:
		if (!ee->in_ack && ee->bit < 8) {
			ee->shift = (u8)((ee->shift << 1) | st_ee_sda_line(ee));
			ee->bit++;
		}
		break;
	case ST_EE_READ:
		if (ee->bit == 8)
			ee->master_ack = st_ee_sda_line(ee) == 0;
		break;
	default:
		break;
	}
}

/* SCL falls: the slave may change SDA now. */
static void
st_ee_fall(struct st_ee *ee)
{
	switch (ee->state) {
	case ST_EE_ADDR:
	case ST_EE_WRITE:
		if (ee->in_ack) {
			/* end of the ACK clock */
			ee->s_sda = 1;
			ee->in_ack = false;
			ee->bit = 0;
			ee->shift = 0;
			if (ee->state == ST_EE_ADDR && ee->rd) {
				ee->state = ST_EE_READ;
				st_ee_send_bit(ee);
			} else if (ee->state == ST_EE_ADDR) {
				ee->state = ST_EE_WRITE;
				ee->ptr_set = false;
			}
		} else if (ee->bit == 8) {
			if (ee->state == ST_EE_ADDR) {
				if ((ee->shift >> 1) != ST_EE_SLAVE) {
					ee->addr_nacks++;
					ee->state = ST_EE_IDLE;
					break;
				}
				ee->rd = (ee->shift & 1) != 0;
			} else if (!ee->ptr_set) {
				ee->ptr = ee->shift;
				ee->ptr_set = true;
			} else {
				ee->mem[ee->ptr++] = ee->shift;
			}
			ee->s_sda = 0;
			ee->in_ack = true;
		}
		break;
	case ST_EE_READ:
		ee->bit++;
		if (ee->bit < 8) {
			st_ee_send_bit(ee);
		} else if (ee->bit == 8) {
			ee->s_sda = 1;		/* the master acknowledges */
		} else {
			ee->ptr++;
			ee->bit = 0;
			if (ee->master_ack) {
				st_ee_send_bit(ee);
			} else {
				ee->s_sda = 1;
				ee->state = ST_EE_IDLE;
			}
		}
		break;
	default:
		break;
	}
}

static void
st_ee_setsda(void *data, int v)
{
	struct st_ee *ee = data;
	int old = st_ee_sda_line(ee);

	ee->m_sda = v != 0;
	if (!st_ee_scl_line(ee) || old == st_ee_sda_line(ee))
		return;
	ee->s_sda = 1;
	ee->in_ack = false;
	ee->bit = 0;
	ee->shift = 0;
	if (old) {
		ee->starts++;
		ee->state = ST_EE_ADDR;
	} else {
		ee->stops++;
		ee->state = ST_EE_IDLE;
	}
}

static void
st_ee_setscl(void *data, int v)
{
	struct st_ee *ee = data;
	int old = st_ee_scl_line(ee);

	ee->m_scl = v != 0;
	if (ee->m_scl)
		ee->stretch_left = ee->stretch_reads;
	if (!old && st_ee_scl_line(ee))
		st_ee_rise(ee);
	else if (old && !st_ee_scl_line(ee))
		st_ee_fall(ee);
}

static int
st_ee_getsda(void *data)
{
	return (st_ee_sda_line(data));
}

static int
st_ee_getscl(void *data)
{
	struct st_ee *ee = data;

	if (ee->scl_stuck)
		return (0);
	if (ee->stretch_left > 0) {
		ee->stretch_left--;
		return (0);
	}
	return (ee->m_scl);
}

static int
st_ee_pre(struct i2c_adapter *adap)
{
	struct st_i2c *t = container_of(adap, struct st_i2c, adap);

	t->ee.pre++;
	return (0);
}

static void
st_ee_post(struct i2c_adapter *adap)
{
	struct st_i2c *t = container_of(adap, struct st_i2c, adap);

	t->ee.post++;
}

static struct st_i2c *
st_i2c_alloc(struct pci_dev *pdev, const char *name)
{
	struct st_i2c *t = kzalloc(sizeof (*t), GFP_KERNEL);

	if (t == NULL)
		return (NULL);
	t->ee.m_sda = t->ee.m_scl = t->ee.s_sda = 1;
	t->bit.data = &t->ee;
	t->bit.setsda = st_ee_setsda;
	t->bit.setscl = st_ee_setscl;
	t->bit.getsda = st_ee_getsda;
	t->bit.getscl = st_ee_getscl;
	t->bit.pre_xfer = st_ee_pre;
	t->bit.post_xfer = st_ee_post;
	t->bit.udelay = 1;
	t->bit.timeout = 5;		/* jiffies (ms) */
	t->adap.algo_data = &t->bit;
	t->adap.dev.parent = pdev != NULL ? &pdev->dev : NULL;
	(void) strscpy(t->adap.name, name, sizeof (t->adap.name));
	return (t);
}

/* --- i2c_del_adapter() waits for references ----------------------------- */

struct st_i2c_del {
	struct i2c_adapter *adap;
	struct completion done;
};

static int
st_i2c_del_fn(void *arg)
{
	struct st_i2c_del *d = arg;

	i2c_del_adapter(d->adap);
	complete(&d->done);
	return (0);
}

/*
 * Deletes t's adapter while a reference is held. False if the deleting
 * helper never returned: t must then stay allocated.
 */
static bool
st_i2c_del_waits(struct st_i2c *t)
{
	static struct st_i2c_del d;	/* outlives a helper left behind */
	struct task_struct *k;
	struct i2c_adapter *ref;

	ref = i2c_get_adapter(t->adap.nr);
	CHECK("i2c: reference for del", ref == &t->adap);
	if (ref != &t->adap) {
		i2c_put_adapter(ref);
		i2c_del_adapter(&t->adap);
		return (true);
	}
	d.adap = &t->adap;
	init_completion(&d.done);
	k = kthread_run(st_i2c_del_fn, &d, "st_i2c_del");
	CHECK("i2c: del helper", !IS_ERR(k));
	if (IS_ERR(k)) {
		i2c_put_adapter(ref);
		i2c_del_adapter(&t->adap);
		return (true);
	}
	CHECK("i2c: del blocks on a reference",
	    wait_for_completion_timeout(&d.done, msecs_to_jiffies(50)) == 0);
	CHECK("i2c: unlisted while deleting",
	    i2c_get_adapter(t->adap.nr) == NULL);
	i2c_put_adapter(ref);
	if (wait_for_completion_timeout(&d.done, msecs_to_jiffies(3000)) == 0) {
		CHECK("i2c: del returns after the put", 0);
		return (false);
	}
	CHECK("i2c: del returns after the put", 1);
	(void) kthread_stop(k);
	return (true);
}

/* --- tests ------------------------------------------------------------- */

static void
st_msg(struct i2c_msg *m, u16 addr, u16 flags, u16 len, u8 *buf)
{
	m->addr = addr;
	m->flags = flags;
	m->len = len;
	m->buf = buf;
}

static void
st_i2c_transfers(struct st_i2c *t)
{
	struct i2c_adapter *a = &t->adap;
	struct st_ee *ee = &t->ee;
	u8 wbuf[4] = { 0x10, 'h', 'i', '!' };
	u8 ptr = 0x10, rbuf[1 + I2C_SMBUS_BLOCK_MAX];
	struct i2c_msg m[2];
	unsigned long start;
	int ret;

	/* Write three bytes at 0x10, then read them back (repeated start). */
	st_msg(&m[0], ST_EE_SLAVE, 0, 4, wbuf);
	ee->pre = ee->post = 0;
	CHECK("i2c: write", i2c_transfer(a, m, 1) == 1);
	CHECK("i2c: slave stored", bcmp(&ee->mem[0x10], "hi!", 3) == 0);
	CHECK("i2c: pre/post_xfer", ee->pre == 1 && ee->post == 1);
	CHECK("i2c: bus idle after write", ee->state == ST_EE_IDLE &&
	    ee->m_sda && ee->m_scl);

	memset(rbuf, 0, sizeof (rbuf));
	st_msg(&m[0], ST_EE_SLAVE, 0, 1, &ptr);
	st_msg(&m[1], ST_EE_SLAVE, I2C_M_RD, 3, rbuf);
	ee->starts = ee->stops = 0;
	CHECK("i2c: write+read", i2c_transfer(a, m, 2) == 2);
	CHECK("i2c: read back", bcmp(rbuf, "hi!", 3) == 0);
	CHECK("i2c: start, repeated start, stop",
	    ee->starts == 2 && ee->stops == 1);

	/* I2C_M_RECV_LEN: the count byte, then that many bytes. */
	ee->mem[0x20] = 3;
	ee->mem[0x21] = 0xa1;
	ee->mem[0x22] = 0xa2;
	ee->mem[0x23] = 0xa3;
	ptr = 0x20;
	memset(rbuf, 0, sizeof (rbuf));
	st_msg(&m[1], ST_EE_SLAVE, I2C_M_RD | I2C_M_RECV_LEN, 1, rbuf);
	CHECK("i2c: recv_len", i2c_transfer(a, m, 2) == 2);
	CHECK("i2c: recv_len length", m[1].len == 4);
	CHECK("i2c: recv_len data", rbuf[0] == 3 && rbuf[1] == 0xa1 &&
	    rbuf[2] == 0xa2 && rbuf[3] == 0xa3);

	/* No device at 0x51: the address is tried 1 + retries times. */
	st_msg(&m[0], ST_EE_SLAVE + 1, 0, 1, &ptr);
	ee->addr_nacks = 0;
	ret = i2c_transfer(a, m, 1);
	CHECK("i2c: NACKed address is -ENXIO", ret == -ENXIO);
	CHECK("i2c: address retries", ee->addr_nacks == 1 + a->retries);
	CHECK("i2c: bus idle after NACK", ee->m_sda && ee->m_scl);
	m[0].flags = I2C_M_IGNORE_NAK;
	m[0].len = 0;
	CHECK("i2c: I2C_M_IGNORE_NAK", i2c_transfer(a, m, 1) == 1);

	/* A stretched clock is waited for. */
	ee->stretch_reads = 20;
	st_msg(&m[0], ST_EE_SLAVE, 0, 1, &ptr);
	st_msg(&m[1], ST_EE_SLAVE, I2C_M_RD, 1, rbuf);
	CHECK("i2c: stretched clock", i2c_transfer(a, m, 2) == 2 &&
	    rbuf[0] == 3);
	ee->stretch_reads = 0;

	/* A clock held low times out (bit.timeout jiffies per wait). */
	ee->scl_stuck = true;
	ee->post = 0;
	start = jiffies;
	ret = i2c_transfer(a, m, 2);
	CHECK("i2c: clock held low is -ETIMEDOUT", ret == -ETIMEDOUT);
	CHECK("i2c: timeout not early",
	    time_after_eq(jiffies, start + t->bit.timeout));
	CHECK("i2c: post_xfer after timeout", ee->post == 1);
	ee->scl_stuck = false;
	/* The slave saw a partial transfer: the next start resets it. */
	m[1].len = 3;
	ptr = 0x10;
	CHECK("i2c: transfer after timeout", i2c_transfer(a, m, 2) == 2 &&
	    bcmp(rbuf, "hi!", 3) == 0);

	/* Invalid arguments are refused, not followed. */
	CHECK("i2c: NULL messages", i2c_transfer(a, NULL, 1) == -EINVAL);
	CHECK("i2c: no messages", i2c_transfer(a, m, 0) == -EINVAL);
	st_msg(&m[0], ST_EE_SLAVE, 0, 2, NULL);
	CHECK("i2c: message without buffer", i2c_transfer(a, m, 1) == -EINVAL);
	st_msg(&m[0], ST_EE_SLAVE, I2C_M_TEN, 1, &ptr);
	CHECK("i2c: 10-bit address refused", i2c_transfer(a, m, 1) == -EINVAL);
}

void
st_i2c_tests(struct pci_dev *pdev)
{
	static struct i2c_adapter none;	/* no algorithm */
	struct st_i2c *a, *b, *c;
	struct i2c_adapter *ref;
	bool locked;
	char name[16];
	int bnr;

	a = st_i2c_alloc(pdev, "st i2c a");
	b = st_i2c_alloc(pdev, "st i2c b");
	c = st_i2c_alloc(NULL, "st i2c c");
	if (a == NULL || b == NULL || c == NULL) {
		CHECK("i2c: allocation", 0);
		goto out;
	}

	CHECK("i2c: add a", i2c_bit_add_bus(&a->adap) == 0);
	CHECK("i2c: add b", i2c_bit_add_bus(&b->adap) == 0);
	CHECK("i2c: distinct numbers", a->adap.nr >= 0 && b->adap.nr >= 0 &&
	    a->adap.nr != b->adap.nr);
	(void) snprintf(name, sizeof (name), "i2c-%d", a->adap.nr);
	CHECK("i2c: device name", strcmp(dev_name(&a->adap.dev), name) == 0);
	CHECK("i2c: defaults", a->adap.retries == 3 && a->adap.timeout == HZ &&
	    a->adap.lock_ops != NULL && a->adap.algo == &i2c_bit_algo);
	CHECK("i2c: functionality",
	    i2c_check_functionality(&a->adap, I2C_FUNC_I2C | I2C_FUNC_NOSTART));
	ref = i2c_get_adapter(a->adap.nr);
	CHECK("i2c_get_adapter", ref == &a->adap);
	i2c_put_adapter(ref);
	CHECK("i2c: add twice is -EBUSY", i2c_add_adapter(&a->adap) == -EBUSY);
	c->adap.nr = a->adap.nr;
	CHECK("i2c: numbered, taken, is -EBUSY",
	    i2c_bit_add_numbered_bus(&c->adap) == -EBUSY);
	CHECK("i2c: no algorithm is -EINVAL",
	    i2c_add_adapter(&none) == -EINVAL);
	CHECK("i2c: transfer without algorithm",
	    i2c_transfer(&none, NULL, 0) == -EOPNOTSUPP);
	i2c_lock_bus(&a->adap, I2C_LOCK_SEGMENT);
	locked = i2c_trylock_bus(&a->adap, I2C_LOCK_SEGMENT) == 0;
	CHECK("i2c: default bus lock", locked);
	if (!locked)
		i2c_unlock_bus(&a->adap, I2C_LOCK_SEGMENT);
	i2c_unlock_bus(&a->adap, I2C_LOCK_SEGMENT);
	CHECK("i2c: bus lock free", i2c_trylock_bus(&a->adap,
	    I2C_LOCK_SEGMENT) != 0);
	i2c_unlock_bus(&a->adap, I2C_LOCK_SEGMENT);

	st_i2c_transfers(a);

	/* The lowest free number is reused. */
	bnr = b->adap.nr;
	i2c_del_adapter(&b->adap);
	CHECK("i2c: deleted adapter gone", i2c_get_adapter(bnr) == NULL);
	c->adap.nr = -1;
	CHECK("i2c: numbered -1", i2c_bit_add_numbered_bus(&c->adap) == 0);
	CHECK("i2c: lowest free number", c->adap.nr == bnr);
	i2c_del_adapter(&c->adap);
	c->adap.nr = bnr;
	CHECK("i2c: numbered, free", i2c_bit_add_numbered_bus(&c->adap) == 0 &&
	    c->adap.nr == bnr);
	i2c_del_adapter(&c->adap);
	i2c_del_adapter(&c->adap);	/* not listed: ignored */

	if (!st_i2c_del_waits(a))
		a = NULL;		/* the helper may still use it */
out:
	kfree(a);
	kfree(b);
	kfree(c);
}
