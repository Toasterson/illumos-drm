/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's tracepoints. Not supported (illumos has DTrace, and the layer
 * adds no SDT probes yet): TRACE_EVENT() and DEFINE_EVENT() declare
 * trace_<name>() as empty inline functions and trace_<name>_enabled() as
 * false, exactly what Linux generates with CONFIG_TRACEPOINTS off.
 */
#ifndef _LINUX_TRACEPOINT_H
#define	_LINUX_TRACEPOINT_H
#include <linux/types.h>
#define	TP_PROTO(args...)		args
#define	TP_ARGS(args...)		args
#define	TP_CONDITION(args...)		args
#define	PARAMS(args...)			args
#define	__lkpi_trace_stub(name, proto) \
	static inline void trace_##name(proto) { } \
	static inline void trace_##name##_rcuidle(proto) { } \
	static inline bool trace_##name##_enabled(void) { return (false); } \
	static inline int register_trace_##name(void *probe, void *data) { return (-ENOSYS); } \
	static inline int unregister_trace_##name(void *probe, void *data) { return (-ENOSYS); }
#define	DECLARE_TRACE(name, proto, args)	__lkpi_trace_stub(name, PARAMS(proto))
#define	TRACE_EVENT(name, proto, args, tstruct, assign, print) \
	__lkpi_trace_stub(name, PARAMS(proto))
#define	TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) \
	__lkpi_trace_stub(name, PARAMS(proto))
#define	DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define	DEFINE_EVENT(template, name, proto, args) \
	__lkpi_trace_stub(name, PARAMS(proto))
#define	DEFINE_EVENT_PRINT(template, name, proto, args, print) \
	__lkpi_trace_stub(name, PARAMS(proto))
#define	DEFINE_EVENT_CONDITION(template, name, proto, args, cond) \
	__lkpi_trace_stub(name, PARAMS(proto))
#define	TRACE_DEFINE_ENUM(a)
#define	TRACE_DEFINE_SIZEOF(a)
#define	tracing_off()			((void)0)
#define	tracing_on()			((void)0)
#define	trace_puts(s)			((void)0)
#define	ftrace_dump(m)			((void)0)
#endif
