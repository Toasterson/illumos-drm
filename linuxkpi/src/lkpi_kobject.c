/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kobjects and the sysfs text helpers (<linux/kobject.h>,
 * <linux/sysfs.h>). illumos has no sysfs, so a kobject is a name, a
 * parent and a reference count: kobject_add() names it, takes a
 * reference on the parent and puts it on the parent's list of children;
 * kobject_del() (and the last kobject_put()) takes it off and drops that
 * reference; the last kobject_put() frees the name and calls the type's
 * release(). sysfs_emit() formats into the page-sized buffer of a show()
 * function, which the drivers' attribute code still calls (e.g. from the
 * layer's debugging aids).
 *
 * The children list exists for kobjects whose memory is not freed by
 * their release(): a layer device's kobject lives in the device, which
 * is freed at the last put_device() (src/lkpi_device.c). A driver may
 * leave kobjects under it (Linux i915 never puts the "engine" kobject
 * it adds under the DRM minor's device); before the device is freed,
 * lkpi_kobject_orphan_children() detaches them, so that no child keeps
 * a pointer to the freed kobject.
 *
 * Locking: lkpi_kobject_lock protects every kobject's parent pointer,
 * lkpi_children and lkpi_sibling. It is a leaf: no other lock is taken,
 * nothing is allocated and no release() runs while it is held, so it
 * orders after any driver mutex. It is an adaptive mutex: as in Linux,
 * kobject_add(), kobject_del() and the put of an added kobject run in
 * thread context, not under a spinlock (a kobject without a parent takes
 * no lock). A child's reference on its parent is dropped under the lock
 * unless it is the parent's last one (then the put, and the parent's
 * release(), follow after the lock is dropped). So when
 * lkpi_kobject_orphan_children() returns, every child's reference on the
 * kobject is gone, by kobject_del() or by the orphaning, and no
 * kobject_del() still to come can touch the kobject: its owner, holding
 * the last reference, may then put and free it.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/compiler.h>
#include <linux/refcount.h>

const char power_group_name[] = "power";

/* Parents and children lists of all kobjects; zero-filled: a valid mutex. */
static kmutex_t lkpi_kobject_lock;

static ssize_t
kobj_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct kobj_attribute *ka = container_of(attr, struct kobj_attribute,
	    attr);

	if (ka->show == NULL)
		return (-EIO);
	return (ka->show(kobj, ka, buf));
}

static ssize_t
kobj_attr_store(struct kobject *kobj, struct attribute *attr,
    const char *buf, size_t count)
{
	struct kobj_attribute *ka = container_of(attr, struct kobj_attribute,
	    attr);

	if (ka->store == NULL)
		return (-EIO);
	return (ka->store(kobj, ka, buf, count));
}

const struct sysfs_ops kobj_sysfs_ops = {
	.show = kobj_attr_show,
	.store = kobj_attr_store
};

void
kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	if (WARN(kobj == NULL, "kobject_init: NULL kobject"))
		return;
	if (kobj->state_initialized)
		WARN(1, "kobject_init: %p already initialised", kobj);
	kobj->name = NULL;
	kobj->parent = NULL;
	kobj->ktype = ktype;
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->lkpi_children);
	INIT_LIST_HEAD(&kobj->lkpi_sibling);
	kobj->state_initialized = 1;
}

static int
kobject_vadd(struct kobject *kobj, struct kobject *parent, const char *fmt,
    va_list ap)
{
	char *name;

	if (kobj == NULL || !kobj->state_initialized) {
		WARN(1, "kobject_add: kobject %p not initialised", kobj);
		return (-EINVAL);
	}
	if (fmt != NULL) {
		name = kvasprintf(GFP_KERNEL, fmt, ap);
		if (name == NULL)
			return (-ENOMEM);
		kfree(kobj->name);
		kobj->name = name;
	}
	if (parent != NULL && !parent->state_initialized) {
		WARN(1, "kobject_add: %s: parent %p not initialised", kobj->name,
		    parent);
		return (-EINVAL);
	}
	mutex_enter(&lkpi_kobject_lock);
	if (kobj->parent != NULL) {
		mutex_exit(&lkpi_kobject_lock);
		WARN(1, "kobject_add: %s already added", kobj->name);
		return (-EINVAL);
	}
	if (parent != NULL) {
		kref_get(&parent->kref);
		list_add_tail(&kobj->lkpi_sibling, &parent->lkpi_children);
	}
	WRITE_ONCE(kobj->parent, parent);
	mutex_exit(&lkpi_kobject_lock);
	return (0);
}

int
kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	va_list ap;
	int err;

	va_start(ap, fmt);
	err = kobject_vadd(kobj, parent, fmt, ap);
	va_end(ap);
	return (err);
}

int
kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
    struct kobject *parent, const char *fmt, ...)
{
	va_list ap;
	int err;

	kobject_init(kobj, ktype);
	va_start(ap, fmt);
	err = kobject_vadd(kobj, parent, fmt, ap);
	va_end(ap);
	return (err);
}

static void
kobject_dynamic_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct kobj_type kobject_dynamic_ktype = {
	.release = kobject_dynamic_release,
	.sysfs_ops = &kobj_sysfs_ops
};

struct kobject *
kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj = kzalloc(sizeof (*kobj), GFP_KERNEL);

	if (kobj == NULL)
		return (NULL);
	kobject_init(kobj, &kobject_dynamic_ktype);
	if (kobject_add(kobj, parent, "%s", name) != 0) {
		kobject_put(kobj);
		return (NULL);
	}
	return (kobj);
}

struct kobject *
kobject_get(struct kobject *kobj)
{
	if (kobj != NULL) {
		if (!kobj->state_initialized)
			WARN(1, "kobject_get: %p not initialised", kobj);
		kref_get(&kobj->kref);
	}
	return (kobj);
}

/*
 * Unlinks the kobject from its parent and drops the parent reference. A
 * kobject without a parent (never added, deleted or orphaned) takes no
 * lock: the parent pointer only becomes non-NULL in kobject_add().
 */
void
kobject_del(struct kobject *kobj)
{
	struct kobject *parent;

	if (kobj == NULL || READ_ONCE(kobj->parent) == NULL)
		return;
	mutex_enter(&lkpi_kobject_lock);
	parent = kobj->parent;
	if (parent != NULL) {
		list_del_init(&kobj->lkpi_sibling);
		WRITE_ONCE(kobj->parent, NULL);
		/* Not the parent's last reference: dropped under the lock. */
		if (refcount_dec_not_one(&parent->kref.refcount))
			parent = NULL;
	}
	mutex_exit(&lkpi_kobject_lock);
	kobject_put(parent);
}

int
lkpi_kobject_orphan_children(struct kobject *kobj)
{
	struct kobject *child, *next;
	int n = 0, last = 0;

	if (kobj == NULL || kobj->lkpi_children.next == NULL)
		return (0);
	mutex_enter(&lkpi_kobject_lock);
	list_for_each_entry_safe(child, next, &kobj->lkpi_children,
	    lkpi_sibling) {
		list_del_init(&child->lkpi_sibling);
		WRITE_ONCE(child->parent, NULL);
		n++;
		/* The caller's own reference keeps the count above one. */
		if (!refcount_dec_not_one(&kobj->kref.refcount))
			last++;
	}
	mutex_exit(&lkpi_kobject_lock);
	WARN(last != 0, "lkpi_kobject_orphan_children: %s (%p): %d child "
	    "references were the last", kobj->name != NULL ? kobj->name :
	    "(unnamed)", kobj, last);
	return (n);
}

static void
kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	const struct kobj_type *t = kobj->ktype;
	const char *name = kobj->name;

	kobject_del(kobj);
	kobj->name = NULL;
	kobj->state_initialized = 0;
	if (t != NULL && t->release != NULL)
		t->release(kobj);
	else
		WARN(1, "kobject %s (%p) has no release function",
		    name != NULL ? name : "(unnamed)", kobj);
	kfree(name);
}

void
kobject_put(struct kobject *kobj)
{
	if (kobj == NULL)
		return;
	if (!kobj->state_initialized) {
		WARN(1, "kobject_put: %p not initialised", kobj);
		return;
	}
	(void) kref_put(&kobj->kref, kobject_release);
}

/* --- sysfs text ---------------------------------------------------------- */

int
sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (WARN(buf == NULL || ((uintptr_t)buf & (PAGE_SIZE - 1)) != 0,
	    "sysfs_emit: invalid buffer %p", buf))
		return (0);
	va_start(ap, fmt);
	n = lkpi_vscnprintf(buf, PAGE_SIZE, fmt, ap);
	va_end(ap);
	return (n);
}

int
sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (WARN(buf == NULL || ((uintptr_t)buf & (PAGE_SIZE - 1)) != 0 ||
	    at < 0 || at >= (int)PAGE_SIZE,
	    "sysfs_emit_at: invalid buffer %p or offset %d", buf, at))
		return (0);
	va_start(ap, fmt);
	n = lkpi_vscnprintf(buf + at, PAGE_SIZE - (size_t)at, fmt, ap);
	va_end(ap);
	return (n);
}
