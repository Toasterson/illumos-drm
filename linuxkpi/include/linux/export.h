/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Exports are the module mapfiles; EXPORT_SYMBOL is empty. */
#ifndef _LINUX_EXPORT_H
#define	_LINUX_EXPORT_H
#define	EXPORT_SYMBOL(sym)
#define	EXPORT_SYMBOL_GPL(sym)
#define	EXPORT_SYMBOL_NS(sym, ns)
#define	EXPORT_SYMBOL_NS_GPL(sym, ns)
#define	EXPORT_SYMBOL_FOR_TESTS_ONLY(sym)
#define	EXPORT_SYMBOL_IF_KUNIT(sym)
#define	VISIBLE_IF_KUNIT	static
#endif
