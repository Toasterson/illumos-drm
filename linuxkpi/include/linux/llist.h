/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's lock-less singly linked lists (include/linux/llist.h, GPL-2.0;
 * reimplemented with compare-and-swap). Producers may add concurrently;
 * llist_del_all() takes the whole list atomically; llist_del_first() needs
 * a single consumer, as on Linux.
 */
#ifndef _LINUX_LLIST_H
#define	_LINUX_LLIST_H

#include <linux/atomic.h>
#include <linux/container_of.h>
#include <linux/stddef.h>

struct llist_head {
	struct llist_node *first;
};

struct llist_node {
	struct llist_node *next;
};

#define	LLIST_HEAD_INIT(name)	{ NULL }
#define	LLIST_HEAD(name)	struct llist_head name = LLIST_HEAD_INIT(name)

static inline void
init_llist_head(struct llist_head *list)
{
	list->first = NULL;
}

static inline void
init_llist_node(struct llist_node *node)
{
	node->next = node;
}

static inline bool
llist_on_list(const struct llist_node *node)
{
	return (node->next != node);
}

#define	llist_entry(ptr, type, member)	container_of(ptr, type, member)
#define	member_address_is_nonnull(ptr, member) \
	((uintptr_t)(ptr) + offsetof(typeof (*(ptr)), member) != 0)
#define	llist_for_each(pos, node) \
	for ((pos) = (node); (pos) != NULL; (pos) = (pos)->next)
#define	llist_for_each_safe(pos, n, node) \
	for ((pos) = (node); (pos) != NULL && ((n) = (pos)->next, true); (pos) = (n))
#define	llist_for_each_entry(pos, node, member) \
	for ((pos) = llist_entry((node), typeof (*(pos)), member); \
	    member_address_is_nonnull(pos, member); \
	    (pos) = llist_entry((pos)->member.next, typeof (*(pos)), member))
#define	llist_for_each_entry_safe(pos, n, node, member) \
	for ((pos) = llist_entry((node), typeof (*(pos)), member); \
	    member_address_is_nonnull(pos, member) && \
	    ((n) = llist_entry((pos)->member.next, typeof (*(n)), member), true); \
	    (pos) = (n))

static inline bool
llist_empty(const struct llist_head *head)
{
	return (READ_ONCE(head->first) == NULL);
}

static inline struct llist_node *
llist_next(struct llist_node *node)
{
	return (READ_ONCE(node->next));
}

static inline bool
llist_add_batch(struct llist_node *new_first, struct llist_node *new_last,
    struct llist_head *head)
{
	struct llist_node *first = READ_ONCE(head->first);

	do {
		new_last->next = first;
	} while (!try_cmpxchg(&head->first, &first, new_first));
	return (first == NULL);
}

static inline bool
__llist_add_batch(struct llist_node *new_first, struct llist_node *new_last,
    struct llist_head *head)
{
	new_last->next = head->first;
	head->first = new_first;
	return (new_last->next == NULL);
}

static inline bool
llist_add(struct llist_node *new_n, struct llist_head *head)
{
	return (llist_add_batch(new_n, new_n, head));
}

static inline bool
__llist_add(struct llist_node *new_n, struct llist_head *head)
{
	return (__llist_add_batch(new_n, new_n, head));
}

static inline struct llist_node *
llist_del_all(struct llist_head *head)
{
	return (xchg(&head->first, NULL));
}

static inline struct llist_node *
__llist_del_all(struct llist_head *head)
{
	struct llist_node *first = head->first;

	head->first = NULL;
	return (first);
}

static inline struct llist_node *
llist_del_first(struct llist_head *head)
{
	struct llist_node *entry = READ_ONCE(head->first), *next;

	do {
		if (entry == NULL)
			return (NULL);
		next = READ_ONCE(entry->next);
	} while (!try_cmpxchg(&head->first, &entry, next));
	return (entry);
}

static inline struct llist_node *
llist_reverse_order(struct llist_node *head)
{
	struct llist_node *new_head = NULL;

	while (head != NULL) {
		struct llist_node *tmp = head;

		head = head->next;
		tmp->next = new_head;
		new_head = tmp;
	}
	return (new_head);
}

#endif
