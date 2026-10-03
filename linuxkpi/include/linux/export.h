/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's EXPORT_SYMBOL() for the carried helpers (carried/). On illumos
 * a module's exported symbols are decided by its symbol-scope mapfile at
 * link time (ld -B reduce -M <module>_scope.mapfile), so the markers
 * expand to nothing.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names follow Linux v7.2's include/linux/export.h
 * (GPL-2.0-only). This file is a reimplementation, not a copy (D7).
 */

#ifndef _LINUXKPI_LINUX_EXPORT_H
#define	_LINUXKPI_LINUX_EXPORT_H

#define	EXPORT_SYMBOL(sym)
#define	EXPORT_SYMBOL_GPL(sym)
#define	EXPORT_SYMBOL_NS(sym, ns)
#define	EXPORT_SYMBOL_NS_GPL(sym, ns)

#endif	/* _LINUXKPI_LINUX_EXPORT_H */
