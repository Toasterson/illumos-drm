/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The kernel debugger hooks: illumos' kmdb is not entered through them;
 * in_dbg_master() is false.
 */
#ifndef _LINUX_KGDB_H
#define	_LINUX_KGDB_H
#define	in_dbg_master()		0
#define	kgdb_breakpoint()	((void)0)
#endif
