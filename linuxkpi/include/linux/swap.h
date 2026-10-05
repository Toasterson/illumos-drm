/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's swap interface. illumos kernel memory is never swapped:
 * total_swap_pages is 0 and the LRU helpers do nothing.
 */
#ifndef _LINUX_SWAP_H
#define	_LINUX_SWAP_H
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/folio_batch.h>
struct swap_iocb;
#define	SWAP_CLUSTER_MAX		32UL
#define	total_swap_pages		0L
#define	get_nr_swap_pages()		0L
#define	lru_add_drain()			((void)0)
#define	lru_add_drain_all()		((void)0)
#define	check_move_unevictable_folios(fb)	((void)(fb))
#define	current_is_kswapd()		false
#define	swap_writeout(f, plug)		0
#endif
