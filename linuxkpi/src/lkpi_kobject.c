/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kobjects and the sysfs text helpers (<linux/kobject.h>,
 * <linux/sysfs.h>). illumos has no sysfs, so a kobject is a name, a
 * parent and a reference count: kobject_add() names it and takes a
 * reference on the parent, the last kobject_put() unlinks it, frees the
 * name and calls the type's release(). sysfs_emit() formats into the
 * page-sized buffer of a show() function, which the drivers' attribute
 * code still calls (e.g. from the layer's debugging aids).
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/bug.h>
#include <linux/errno.h>

const char power_group_name[] = "power";

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
	if (kobj->parent != NULL) {
		WARN(1, "kobject_add: %s already added", kobj->name);
		return (-EINVAL);
	}
	kobj->parent = kobject_get(parent);
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

/* Unlinks the kobject from its parent (dropping the parent reference). */
void
kobject_del(struct kobject *kobj)
{
	struct kobject *parent;

	if (kobj == NULL)
		return;
	parent = kobj->parent;
	kobj->parent = NULL;
	kobject_put(parent);
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
