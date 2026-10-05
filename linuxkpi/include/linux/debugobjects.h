/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's debug-object tracking. Not supported (a debugging aid): the
 * hooks do nothing and report success.
 */
#ifndef _LINUX_DEBUGOBJECTS_H
#define	_LINUX_DEBUGOBJECTS_H
#include <linux/types.h>
struct debug_obj_descr {
	const char *name;
};
enum debug_obj_state {
	ODEBUG_STATE_NONE,
	ODEBUG_STATE_INIT,
	ODEBUG_STATE_INACTIVE,
	ODEBUG_STATE_ACTIVE,
	ODEBUG_STATE_DESTROYED,
	ODEBUG_STATE_NOTAVAILABLE
};
#define	debug_object_init(a, d)		((void)(a))
#define	debug_object_init_on_stack(a, d)	((void)(a))
#define	debug_object_activate(a, d)	0
#define	debug_object_deactivate(a, d)	((void)(a))
#define	debug_object_destroy(a, d)	((void)(a))
#define	debug_object_free(a, d)		((void)(a))
#define	debug_object_assert_init(a, d)	((void)(a))
#define	debug_check_no_obj_freed(a, s)	((void)(a))
#endif
