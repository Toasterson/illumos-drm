/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sysctl tables. Not supported (no /proc/sys): register_sysctl()
 * returns NULL (callers treat it as "no sysctl"), unregistration does
 * nothing.
 */
#ifndef _LINUX_SYSCTL_H
#define	_LINUX_SYSCTL_H
#include <linux/types.h>
struct ctl_table;
struct ctl_table_header;
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	umode_t mode;
	int (*proc_handler)(const struct ctl_table *ctl, int write, void *buffer, size_t *lenp, loff_t *ppos);
	void *extra1;
	void *extra2;
};
#define	register_sysctl(path, table)	((struct ctl_table_header *)NULL)
#define	register_sysctl_sz(p, t, s)	((struct ctl_table_header *)NULL)
#define	unregister_sysctl_table(h)	((void)(h))
#define	proc_dointvec_minmax		NULL
#define	proc_dointvec			NULL
#define	SYSCTL_ZERO			((void *)NULL)
#define	SYSCTL_ONE			((void *)NULL)
#endif
