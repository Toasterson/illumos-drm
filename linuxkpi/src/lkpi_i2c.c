/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's I2C core for bus drivers (<linux/i2c.h>): the adapter list with
 * bus numbers, adapter references, the default bus lock and message
 * transfers with retries. Independent implementation of the interface.
 *
 * The registered adapters are kept on one list sorted by bus number,
 * under a global lock, so that the lowest free number is found in one
 * walk. i2c_get_adapter() references are counted in the adapter
 * (lkpi_refs, under the global lock); i2c_del_adapter() unlists the
 * adapter and sleeps on the global condition variable until the count is
 * 0. The global lock and condition variable are module-static and
 * zero-filled, which illumos accepts as initialised (as lkpi_kthread.c's
 * lock), so this part needs no init/fini.
 *
 * The adapter's struct device is a layer device (lkpi_device_init()) on
 * the parent's dev_info_t, named "i2c-<nr>": dev_err() on the adapter
 * then reports the GPU's instance, and the device has the kobject and
 * devres list other layer code expects of any struct device. The adapter
 * itself belongs to the driver (embedded in its structures): the core
 * never frees it, and lkpi_device_fini() at i2c_del_adapter() releases
 * only what the device holds.
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sprintf.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

static kmutex_t lkpi_i2c_lock;		/* zero-filled: a valid mutex */
static kcondvar_t lkpi_i2c_cv;		/* zero-filled: a valid cv */
/* Listed adapters, ascending adap->nr; lkpi_refs; under lkpi_i2c_lock. */
static struct list_head lkpi_i2c_adapters =
    LIST_HEAD_INIT(lkpi_i2c_adapters);

/* --- default lock operations ------------------------------------------- */

/* One lock per adapter: the flags (root adapter, segment) do not matter. */
static void
lkpi_i2c_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void) flags;
	mutex_lock(&adap->bus_lock);
}

static int
lkpi_i2c_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void) flags;
	return (mutex_trylock(&adap->bus_lock));
}

static void
lkpi_i2c_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void) flags;
	mutex_unlock(&adap->bus_lock);
}

static const struct i2c_lock_operations lkpi_i2c_default_lock_ops = {
	.lock_bus = lkpi_i2c_lock_bus,
	.trylock_bus = lkpi_i2c_trylock_bus,
	.unlock_bus = lkpi_i2c_unlock_bus
};

/* --- adapter list ------------------------------------------------------ */

/*
 * Registers adap with bus number nr, or the lowest free one if nr is -1.
 * The adapter is initialised under the global lock before it is listed,
 * so that i2c_get_adapter() never returns a half-initialised adapter
 * (lkpi_device_init() may sleep in kmem_alloc; an adaptive kmutex may be
 * held across that).
 */
static int
lkpi_i2c_register(struct i2c_adapter *adap, int nr)
{
	struct i2c_adapter *pos;
	struct list_head *before;
	struct device *parent;
	char name[sizeof (adap->dev.lkpi_name)];

	if (WARN(adap == NULL, "i2c: registering a NULL adapter"))
		return (-EINVAL);
	if (adap->algo == NULL) {
		pr_err("i2c: adapter '%s': no algorithm\n", adap->name);
		return (-EINVAL);
	}
	if (nr < -1)
		return (-EINVAL);
	might_sleep();

	mutex_enter(&lkpi_i2c_lock);
	if (adap->lkpi_listed) {
		mutex_exit(&lkpi_i2c_lock);
		WARN(1, "i2c: adapter '%s' (i2c-%d) registered twice",
		    adap->name, adap->nr);
		return (-EBUSY);
	}

	/* The insertion point keeps the list sorted by number. */
	before = &lkpi_i2c_adapters;
	if (nr == -1) {
		nr = 0;
		list_for_each_entry(pos, &lkpi_i2c_adapters, lkpi_link) {
			if (pos->nr > nr) {
				before = &pos->lkpi_link;
				break;
			}
			if (pos->nr == INT_MAX) {
				/* every number up to INT_MAX is taken */
				mutex_exit(&lkpi_i2c_lock);
				return (-ENOSPC);
			}
			nr = pos->nr + 1;
		}
	} else {
		list_for_each_entry(pos, &lkpi_i2c_adapters, lkpi_link) {
			if (pos->nr == nr) {
				mutex_exit(&lkpi_i2c_lock);
				return (-EBUSY);
			}
			if (pos->nr > nr) {
				before = &pos->lkpi_link;
				break;
			}
		}
	}

	adap->nr = nr;
	if (adap->timeout == 0)
		adap->timeout = HZ;
	mutex_init(&adap->bus_lock);
	if (adap->lock_ops == NULL)
		adap->lock_ops = &lkpi_i2c_default_lock_ops;
	(void) snprintf(name, sizeof (name), "i2c-%d", nr);
	parent = adap->dev.parent;
	lkpi_device_init(&adap->dev, parent != NULL ? parent->lkpi_dip : NULL,
	    name);
	adap->lkpi_refs = 0;
	adap->lkpi_listed = true;
	list_add_tail(&adap->lkpi_link, before);
	mutex_exit(&lkpi_i2c_lock);
	return (0);
}

int
i2c_add_adapter(struct i2c_adapter *adap)
{
	return (lkpi_i2c_register(adap, -1));
}

int
i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	if (WARN(adap == NULL, "i2c: registering a NULL adapter"))
		return (-EINVAL);
	return (lkpi_i2c_register(adap, adap->nr));
}

void
i2c_del_adapter(struct i2c_adapter *adap)
{
	if (adap == NULL)
		return;
	might_sleep();

	mutex_enter(&lkpi_i2c_lock);
	if (!adap->lkpi_listed) {
		mutex_exit(&lkpi_i2c_lock);
		pr_debug("i2c: deleting adapter '%s', which is not "
		    "registered\n", adap->name);
		return;
	}
	list_del(&adap->lkpi_link);
	adap->lkpi_listed = false;
	while (adap->lkpi_refs != 0)
		cv_wait(&lkpi_i2c_cv, &lkpi_i2c_lock);
	mutex_exit(&lkpi_i2c_lock);

	/*
	 * illumos panics on destroying a mutex another thread holds: wait
	 * for a transfer still holding the bus lock first. The deleting
	 * thread holding it itself is a driver bug; the mutex is then left
	 * as it is (an adaptive kmutex owns no resources).
	 */
	if (!WARN(mutex_owned(&adap->bus_lock.m),
	    "i2c: deleting adapter %s with its bus lock held",
	    dev_name(&adap->dev))) {
		mutex_lock(&adap->bus_lock);
		mutex_unlock(&adap->bus_lock);
		mutex_destroy(&adap->bus_lock);
	}
	lkpi_device_fini(&adap->dev);
}

struct i2c_adapter *
i2c_get_adapter(int nr)
{
	struct i2c_adapter *pos, *found = NULL;

	mutex_enter(&lkpi_i2c_lock);
	list_for_each_entry(pos, &lkpi_i2c_adapters, lkpi_link) {
		if (pos->nr == nr) {
			found = pos;
			found->lkpi_refs++;
			break;
		}
		if (pos->nr > nr)
			break;
	}
	mutex_exit(&lkpi_i2c_lock);
	return (found);
}

void
i2c_put_adapter(struct i2c_adapter *adap)
{
	if (adap == NULL)
		return;
	mutex_enter(&lkpi_i2c_lock);
	if (adap->lkpi_refs == 0) {
		mutex_exit(&lkpi_i2c_lock);
		WARN(1, "i2c: i2c_put_adapter() of %s without a reference",
		    adap->name);
		return;
	}
	if (--adap->lkpi_refs == 0)
		cv_broadcast(&lkpi_i2c_cv);
	mutex_exit(&lkpi_i2c_lock);
}

/* --- transfers --------------------------------------------------------- */

/*
 * The adapter's plain quirk limits (no I2C_AQ_* combined-message flags:
 * those are not declared, an adapter with flags set is not checked).
 */
static int
lkpi_i2c_check_quirks(const struct i2c_adapter *adap,
    const struct i2c_msg *msgs, int num)
{
	const struct i2c_adapter_quirks *q = adap->quirks;
	int i;

	if (q == NULL || q->flags != 0)
		return (0);
	if (q->max_num_msgs != 0 && num > q->max_num_msgs)
		return (-EOPNOTSUPP);
	for (i = 0; i < num; i++) {
		u16 max = (msgs[i].flags & I2C_M_RD) != 0 ?
		    q->max_read_len : q->max_write_len;

		if (max != 0 && msgs[i].len > max)
			return (-EOPNOTSUPP);
	}
	return (0);
}

int
__i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	unsigned long start;
	int i, try, ret;

	if (WARN(adap == NULL, "i2c: transfer on a NULL adapter"))
		return (-EINVAL);
	if (adap->algo == NULL || adap->algo->master_xfer == NULL)
		return (-EOPNOTSUPP);
	if (WARN(msgs == NULL || num < 1,
	    "i2c: %s: transfer of %d messages at %p", adap->name, num, msgs))
		return (-EINVAL);
	for (i = 0; i < num; i++) {
		if (msgs[i].len > 0 && msgs[i].buf == NULL) {
			WARN(1, "i2c: %s: message %d without buffer",
			    adap->name, i);
			return (-EINVAL);
		}
	}
	if ((ret = lkpi_i2c_check_quirks(adap, msgs, num)) != 0)
		return (ret);

	/* -EAGAIN (lost arbitration, busy): again, within the time limit. */
	start = jiffies;
	for (try = 0; ; try++) {
		ret = adap->algo->master_xfer(adap, msgs, num);
		if (ret != -EAGAIN || try >= adap->retries ||
		    time_after(jiffies, start + adap->timeout))
			break;
	}
	return (ret);
}

int
i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	const struct i2c_lock_operations *ops;
	int ret;

	if (WARN(adap == NULL, "i2c: transfer on a NULL adapter"))
		return (-EINVAL);
	if (adap->algo == NULL || adap->algo->master_xfer == NULL)
		return (-EOPNOTSUPP);
	/* The bus lock sleeps: never from an interrupt handler. */
	if (WARN_ONCE(in_interrupt(), "i2c: %s: transfer in interrupt context",
	    adap->name))
		return (-EAGAIN);
	might_sleep();

	ops = adap->lock_ops;
	if (ops != NULL && (ops->lock_bus == NULL || ops->unlock_bus == NULL)) {
		WARN(1, "i2c: %s: incomplete lock operations", adap->name);
		return (-EINVAL);
	}
	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(adap, msgs, num);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return (ret);
}
