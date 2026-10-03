/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's circular doubly linked lists and hash lists (include/linux/
 * list.h, GPL-2.0; reimplemented on the facade's struct list_head, with
 * FreeBSD's LinuxKPI list.h (BSD-2-Clause) as a structural reference).
 * Deleted entries are poisoned with LIST_POISON1/2 as on Linux.
 * illumos' <sys/list.h> has a list_move_tail() of its own; the Linux one
 * is renamed with a macro.
 */
#ifndef _LINUX_LIST_H
#define	_LINUX_LIST_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/poison.h>
#include <linux/compiler.h>
#include <linux/container_of.h>

#define	list_move_tail	lkpi_list_move_tail

#define	LIST_HEAD_INIT(name)	{ &(name), &(name) }
#define	LIST_HEAD(name)		struct list_head name = LIST_HEAD_INIT(name)

static inline void
INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}

static inline void
__list_add(struct list_head *entry, struct list_head *prev,
    struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	WRITE_ONCE(prev->next, entry);
}

static inline void
list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}

static inline void
list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head->prev, head);
}

static inline void
__list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
}

static inline void
__list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void
list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = (struct list_head *)LIST_POISON1;
	entry->prev = (struct list_head *)LIST_POISON2;
}

static inline void
list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

static inline void
list_replace(struct list_head *old, struct list_head *new_e)
{
	new_e->next = old->next;
	new_e->next->prev = new_e;
	new_e->prev = old->prev;
	new_e->prev->next = new_e;
}

static inline void
list_replace_init(struct list_head *old, struct list_head *new_e)
{
	list_replace(old, new_e);
	INIT_LIST_HEAD(old);
}

static inline void
list_swap(struct list_head *e1, struct list_head *e2)
{
	struct list_head *pos = e2->prev;

	list_del(e2);
	list_replace(e1, e2);
	if (pos == e1)
		pos = e2;
	list_add(e1, pos);
}

static inline void
list_move(struct list_head *entry, struct list_head *head)
{
	__list_del_entry(entry);
	list_add(entry, head);
}

static inline void
lkpi_list_move_tail(struct list_head *entry, struct list_head *head)
{
	__list_del_entry(entry);
	list_add_tail(entry, head);
}

static inline void
list_bulk_move_tail(struct list_head *head, struct list_head *first,
    struct list_head *last)
{
	first->prev->next = last->next;
	last->next->prev = first->prev;
	head->prev->next = first;
	first->prev = head->prev;
	last->next = head;
	head->prev = last;
}

static inline int
list_is_first(const struct list_head *list, const struct list_head *head)
{
	return (list->prev == head);
}

static inline int
list_is_last(const struct list_head *list, const struct list_head *head)
{
	return (list->next == head);
}

static inline int
list_is_head(const struct list_head *list, const struct list_head *head)
{
	return (list == head);
}

static inline int
list_empty(const struct list_head *head)
{
	return (READ_ONCE(head->next) == head);
}

static inline int
list_empty_careful(const struct list_head *head)
{
	struct list_head *next = head->next;

	return (next == head && next == head->prev);
}

static inline int
list_is_singular(const struct list_head *head)
{
	return (!list_empty(head) && head->next == head->prev);
}

static inline void
list_rotate_left(struct list_head *head)
{
	if (!list_empty(head))
		lkpi_list_move_tail(head->next, head);
}

static inline void
list_rotate_to_front(struct list_head *list, struct list_head *head)
{
	lkpi_list_move_tail(head, list);
}

static inline size_t
list_count_nodes(struct list_head *head)
{
	struct list_head *pos;
	size_t count = 0;

	for (pos = head->next; pos != head; pos = pos->next)
		count++;
	return (count);
}

static inline void
__list_cut_position(struct list_head *list, struct list_head *head,
    struct list_head *entry)
{
	struct list_head *new_first = entry->next;

	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

static inline void
list_cut_position(struct list_head *list, struct list_head *head,
    struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (list_is_singular(head) && head->next != entry && head != entry)
		return;
	if (entry == head)
		INIT_LIST_HEAD(list);
	else
		__list_cut_position(list, head, entry);
}

static inline void
list_cut_before(struct list_head *list, struct list_head *head,
    struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}

static inline void
__list_splice(const struct list_head *list, struct list_head *prev,
    struct list_head *next)
{
	struct list_head *first = list->next;
	struct list_head *last = list->prev;

	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}

static inline void
list_splice(const struct list_head *list, struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}

static inline void
list_splice_tail(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head->prev, head);
}

static inline void
list_splice_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}

static inline void
list_splice_tail_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

#define	list_entry(ptr, type, member)	container_of(ptr, type, member)
#define	list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)
#define	list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)
#define	list_first_entry_or_null(ptr, type, member) ({ \
	struct list_head *head__ = (ptr); \
	struct list_head *pos__ = READ_ONCE(head__->next); \
	pos__ != head__ ? list_entry(pos__, type, member) : NULL; })
#define	list_last_entry_or_null(ptr, type, member) ({ \
	struct list_head *head__ = (ptr); \
	struct list_head *pos__ = READ_ONCE(head__->prev); \
	pos__ != head__ ? list_entry(pos__, type, member) : NULL; })
#define	list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof (*(pos)), member)
#define	list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof (*(pos)), member)
#define	list_next_entry_circular(pos, head, member) \
	(list_is_last(&(pos)->member, head) ? \
	list_first_entry(head, typeof (*(pos)), member) : \
	list_next_entry(pos, member))
#define	list_prev_entry_circular(pos, head, member) \
	(list_is_first(&(pos)->member, head) ? \
	list_last_entry(head, typeof (*(pos)), member) : \
	list_prev_entry(pos, member))
#define	list_entry_is_head(pos, head, member)	(&(pos)->member == (head))

#define	list_for_each(pos, head) \
	for ((pos) = (head)->next; !list_is_head((pos), (head)); \
	    (pos) = (pos)->next)
#define	list_for_each_prev(pos, head) \
	for ((pos) = (head)->prev; !list_is_head((pos), (head)); \
	    (pos) = (pos)->prev)
#define	list_for_each_safe(pos, n, head) \
	for ((pos) = (head)->next, (n) = (pos)->next; \
	    !list_is_head((pos), (head)); (pos) = (n), (n) = (pos)->next)
#define	list_for_each_prev_safe(pos, n, head) \
	for ((pos) = (head)->prev, (n) = (pos)->prev; \
	    !list_is_head((pos), (head)); (pos) = (n), (n) = (pos)->prev)
#define	list_for_each_continue(pos, head) \
	for ((pos) = (pos)->next; !list_is_head((pos), (head)); \
	    (pos) = (pos)->next)
#define	list_for_each_entry(pos, head, member) \
	for ((pos) = list_first_entry(head, typeof (*(pos)), member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = list_next_entry(pos, member))
#define	list_for_each_entry_reverse(pos, head, member) \
	for ((pos) = list_last_entry(head, typeof (*(pos)), member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = list_prev_entry(pos, member))
#define	list_prepare_entry(pos, head, member) \
	((pos) ? : list_entry(head, typeof (*(pos)), member))
#define	list_for_each_entry_continue(pos, head, member) \
	for ((pos) = list_next_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = list_next_entry(pos, member))
#define	list_for_each_entry_continue_reverse(pos, head, member) \
	for ((pos) = list_prev_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = list_prev_entry(pos, member))
#define	list_for_each_entry_from(pos, head, member) \
	for (; !list_entry_is_head(pos, head, member); \
	    (pos) = list_next_entry(pos, member))
#define	list_for_each_entry_from_reverse(pos, head, member) \
	for (; !list_entry_is_head(pos, head, member); \
	    (pos) = list_prev_entry(pos, member))
#define	list_for_each_entry_safe(pos, n, head, member) \
	for ((pos) = list_first_entry(head, typeof (*(pos)), member), \
	    (n) = list_next_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = (n), (n) = list_next_entry(n, member))
#define	list_for_each_entry_safe_continue(pos, n, head, member) \
	for ((pos) = list_next_entry(pos, member), \
	    (n) = list_next_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = (n), (n) = list_next_entry(n, member))
#define	list_for_each_entry_safe_from(pos, n, head, member) \
	for ((n) = list_next_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = (n), (n) = list_next_entry(n, member))
#define	list_for_each_entry_safe_reverse(pos, n, head, member) \
	for ((pos) = list_last_entry(head, typeof (*(pos)), member), \
	    (n) = list_prev_entry(pos, member); \
	    !list_entry_is_head(pos, head, member); \
	    (pos) = (n), (n) = list_prev_entry(n, member))
#define	list_safe_reset_next(pos, n, member) \
	((n) = list_next_entry(pos, member))

/* --- hash lists ---------------------------------------------------------- */

#define	HLIST_HEAD_INIT		{ .first = NULL }
#define	HLIST_HEAD(name)	struct hlist_head name = { .first = NULL }
#define	INIT_HLIST_HEAD(ptr)	((ptr)->first = NULL)

static inline void
INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

static inline int
hlist_unhashed(const struct hlist_node *h)
{
	return (h->pprev == NULL);
}

static inline int
hlist_empty(const struct hlist_head *h)
{
	return (READ_ONCE(h->first) == NULL);
}

static inline void
__hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;

	WRITE_ONCE(*pprev, next);
	if (next != NULL)
		next->pprev = pprev;
}

static inline void
hlist_del(struct hlist_node *n)
{
	__hlist_del(n);
	n->next = (struct hlist_node *)LIST_POISON1;
	n->pprev = (struct hlist_node **)LIST_POISON2;
}

static inline void
hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

static inline void
hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first;
	if (first != NULL)
		first->pprev = &n->next;
	WRITE_ONCE(h->first, n);
	n->pprev = &h->first;
}

static inline void
hlist_add_before(struct hlist_node *n, struct hlist_node *next)
{
	n->pprev = next->pprev;
	n->next = next;
	next->pprev = &n->next;
	WRITE_ONCE(*(n->pprev), n);
}

static inline void
hlist_add_behind(struct hlist_node *n, struct hlist_node *prev)
{
	n->next = prev->next;
	prev->next = n;
	n->pprev = &prev->next;
	if (n->next != NULL)
		n->next->pprev = &n->next;
}

static inline void
hlist_move_list(struct hlist_head *old, struct hlist_head *new_h)
{
	new_h->first = old->first;
	if (new_h->first != NULL)
		new_h->first->pprev = &new_h->first;
	old->first = NULL;
}

#define	hlist_entry(ptr, type, member)	container_of(ptr, type, member)
#define	hlist_entry_safe(ptr, type, member) ({ \
	typeof (ptr) ____ptr = (ptr); \
	____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define	hlist_for_each(pos, head) \
	for ((pos) = (head)->first; (pos) != NULL; (pos) = (pos)->next)
#define	hlist_for_each_safe(pos, n, head) \
	for ((pos) = (head)->first; (pos) != NULL && \
	    ({ (n) = (pos)->next; 1; }); (pos) = (n))
#define	hlist_for_each_entry(pos, head, member) \
	for ((pos) = hlist_entry_safe((head)->first, typeof (*(pos)), member); \
	    (pos) != NULL; \
	    (pos) = hlist_entry_safe((pos)->member.next, typeof (*(pos)), member))
#define	hlist_for_each_entry_continue(pos, member) \
	for ((pos) = hlist_entry_safe((pos)->member.next, typeof (*(pos)), member); \
	    (pos) != NULL; \
	    (pos) = hlist_entry_safe((pos)->member.next, typeof (*(pos)), member))
#define	hlist_for_each_entry_safe(pos, n, head, member) \
	for ((pos) = hlist_entry_safe((head)->first, typeof (*(pos)), member); \
	    (pos) != NULL && ({ (n) = (pos)->member.next; 1; }); \
	    (pos) = hlist_entry_safe(n, typeof (*(pos)), member))

extern void list_sort(void *priv, struct list_head *head,
    int (*cmp)(void *priv, const struct list_head *a,
    const struct list_head *b));

#endif
