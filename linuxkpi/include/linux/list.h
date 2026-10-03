/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's circular doubly linked lists (struct list_head) for the carried
 * helpers (carried/). The structure itself is the facade's
 * (drm/drm_facade_types.h), so lists may cross the facade.
 *
 * __list_del_entry() unlinks an entry and leaves its own pointers as they
 * were (drm_mm's eviction scan relies on that to put a node back);
 * list_del() also poisons them with non-canonical addresses, so a later
 * use faults at once.
 *
 * Not here: Linux's list_move_tail(). The illumos kernel has its own
 * list_move_tail(list_t *, list_t *) in sys/list.h, which the facade's
 * system headers always bring in first, so the Linux one can only exist
 * as a function-like macro over another name (R5c's decision; the carried
 * helpers do not use it).
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's include/linux/list.h
 * (GPL-2.0). This file is a reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_LIST_H
#define	_LINUXKPI_LINUX_LIST_H

#include <linux/types.h>
#include <linux/compiler.h>

#define	LIST_POISON1	((struct list_head *)0xdeadbeef00000100UL)
#define	LIST_POISON2	((struct list_head *)0xdeadbeef00000200UL)

#define	LIST_HEAD_INIT(name)	{ &(name), &(name) }
#define	LIST_HEAD(name)		struct list_head name = LIST_HEAD_INIT(name)

static inline void
INIT_LIST_HEAD(struct list_head *head)
{
	WRITE_ONCE(head->next, head);
	head->prev = head;
}

/* Links `entry` between the adjacent entries `prev` and `next`. */
static inline void
__lkpi_list_link(struct list_head *entry, struct list_head *prev,
    struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	WRITE_ONCE(prev->next, entry);
}

/* Inserts `entry` right after `head` (stack order). */
static inline void
list_add(struct list_head *entry, struct list_head *head)
{
	__lkpi_list_link(entry, head, head->next);
}

/* Inserts `entry` right before `head` (queue order). */
static inline void
list_add_tail(struct list_head *entry, struct list_head *head)
{
	__lkpi_list_link(entry, head->prev, head);
}

/* Joins `prev` and `next`, dropping whatever was between them. */
static inline void
__list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
}

/* Unlinks `entry`; its own pointers keep their values. */
static inline void
__list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void
list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline void
list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

static inline void
list_move(struct list_head *entry, struct list_head *head)
{
	__list_del_entry(entry);
	list_add(entry, head);
}

static inline bool
list_empty(const struct list_head *head)
{
	return (READ_ONCE(head->next) == head);
}

static inline bool
list_is_first(const struct list_head *entry, const struct list_head *head)
{
	return (entry->prev == head);
}

static inline bool
list_is_last(const struct list_head *entry, const struct list_head *head)
{
	return (entry->next == head);
}

#define	list_entry(ptr, type, member)	container_of(ptr, type, member)
#define	list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)
#define	list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)
#define	list_first_entry_or_null(head, type, member) __extension__ ({	\
	struct list_head *__lkpi_lh = (head);				\
	struct list_head *__lkpi_ln = READ_ONCE(__lkpi_lh->next);	\
	__lkpi_ln != __lkpi_lh ? list_entry(__lkpi_ln, type, member) : NULL; \
})
#define	list_next_entry(pos, member) \
	list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define	list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define	list_entry_is_head(pos, head, member)	(&(pos)->member == (head))

#define	list_for_each(pos, head) \
	for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define	list_for_each_safe(pos, n, head)				\
	for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head);	\
	    (pos) = (n), (n) = (pos)->next)
#define	list_for_each_entry(pos, head, member)				\
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member); \
	    !list_entry_is_head(pos, head, member);			\
	    (pos) = list_next_entry(pos, member))
#define	list_for_each_entry_reverse(pos, head, member)			\
	for ((pos) = list_last_entry(head, __typeof__(*(pos)), member);	\
	    !list_entry_is_head(pos, head, member);			\
	    (pos) = list_prev_entry(pos, member))
#define	list_for_each_entry_safe(pos, n, head, member)			\
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member), \
	    (n) = list_next_entry(pos, member);				\
	    !list_entry_is_head(pos, head, member);			\
	    (pos) = (n), (n) = list_next_entry(n, member))

#endif	/* _LINUXKPI_LINUX_LIST_H */
