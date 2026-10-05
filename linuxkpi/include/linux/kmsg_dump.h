/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kernel-log dumpers (panic screens). Not supported on illumos:
 * registration fails with -ENODEV.
 */
#ifndef _LINUX_KMSG_DUMP_H
#define	_LINUX_KMSG_DUMP_H
#include <linux/list.h>
enum kmsg_dump_reason {
	KMSG_DUMP_UNDEF,
	KMSG_DUMP_PANIC,
	KMSG_DUMP_OOPS,
	KMSG_DUMP_EMERG,
	KMSG_DUMP_SHUTDOWN,
	KMSG_DUMP_MAX
};
struct kmsg_dump_detail {
	enum kmsg_dump_reason reason;
	const char *description;
};
struct kmsg_dumper {
	struct list_head list;
	void (*dump)(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail);
	enum kmsg_dump_reason max_reason;
	bool registered;
};
#define	kmsg_dump_register(d)	(-ENODEV)
#define	kmsg_dump_unregister(d)	(-EINVAL)
#endif
