/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's notifier chains (<linux/notifier.h>): singly linked lists of
 * notifier blocks in descending priority (equal priorities in order of
 * registration). Blocking chains are called under the chain's rw
 * semaphore as reader and changed as writer; atomic chains are called and
 * changed under the chain's spinlock, so their callbacks must not sleep.
 * A callback must not change the chain it is called from.
 */

#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

static int
lkpi_notifier_insert(struct notifier_block **list, struct notifier_block *nb)
{
	struct notifier_block **pp;

	for (pp = list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == nb) {
			WARN(1, "linuxkpi: notifier registered twice");
			return (-EEXIST);
		}
	}
	for (pp = list; *pp != NULL; pp = &(*pp)->next) {
		if (nb->priority > (*pp)->priority)
			break;
	}
	nb->next = *pp;
	*pp = nb;
	return (0);
}

static int
lkpi_notifier_remove(struct notifier_block **list, struct notifier_block *nb)
{
	struct notifier_block **pp;

	for (pp = list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == nb) {
			*pp = nb->next;
			return (0);
		}
	}
	return (-ENOENT);
}

static int
lkpi_notifier_call(struct notifier_block *nb, unsigned long val, void *v)
{
	struct notifier_block *next;
	int ret = NOTIFY_DONE;

	for (; nb != NULL; nb = next) {
		next = nb->next;
		ret = nb->notifier_call(nb, val, v);
		if ((ret & NOTIFY_STOP_MASK) != 0)
			break;
	}
	return (ret);
}

int
blocking_notifier_chain_register(struct blocking_notifier_head *nh,
    struct notifier_block *nb)
{
	int ret;

	down_write(&nh->rwsem);
	ret = lkpi_notifier_insert(&nh->head, nb);
	up_write(&nh->rwsem);
	return (ret);
}

int
blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
    struct notifier_block *nb)
{
	int ret;

	down_write(&nh->rwsem);
	ret = lkpi_notifier_remove(&nh->head, nb);
	up_write(&nh->rwsem);
	return (ret);
}

int
blocking_notifier_call_chain(struct blocking_notifier_head *nh,
    unsigned long val, void *v)
{
	int ret = NOTIFY_DONE;

	/* Like Linux, skip the semaphore for an empty chain. */
	if (READ_ONCE(nh->head) == NULL)
		return (ret);
	down_read(&nh->rwsem);
	ret = lkpi_notifier_call(nh->head, val, v);
	up_read(&nh->rwsem);
	return (ret);
}

int
atomic_notifier_chain_register(struct atomic_notifier_head *nh,
    struct notifier_block *nb)
{
	int ret;

	spin_lock(&nh->lock);
	ret = lkpi_notifier_insert(&nh->head, nb);
	spin_unlock(&nh->lock);
	return (ret);
}

int
atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
    struct notifier_block *nb)
{
	int ret;

	spin_lock(&nh->lock);
	ret = lkpi_notifier_remove(&nh->head, nb);
	spin_unlock(&nh->lock);
	return (ret);
}

int
atomic_notifier_call_chain(struct atomic_notifier_head *nh,
    unsigned long val, void *v)
{
	int ret;

	spin_lock(&nh->lock);
	ret = lkpi_notifier_call(nh->head, val, v);
	spin_unlock(&nh->lock);
	return (ret);
}
