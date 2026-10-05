/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's RCU list operations (include/linux/rculist.h, GPL-2.0;
 * reimplemented): the update side publishes with a release store and
 * deletion keeps the forward pointer intact for concurrent readers.
 */
#ifndef _LINUX_RCULIST_H
#define	_LINUX_RCULIST_H

#include <linux/list.h>
#include <linux/rcupdate.h>

static inline void
list_add_rcu(struct list_head *entry, struct list_head *head)
{
	struct list_head *next = head->next;

	entry->next = next;
	entry->prev = head;
	smp_store_release(&head->next, entry);
	next->prev = entry;
}

static inline void
list_add_tail_rcu(struct list_head *entry, struct list_head *head)
{
	struct list_head *prev = head->prev;

	entry->next = head;
	entry->prev = prev;
	smp_store_release(&prev->next, entry);
	head->prev = entry;
}

static inline void
list_del_rcu(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->prev = (struct list_head *)LIST_POISON2;
}

static inline void
list_replace_rcu(struct list_head *old, struct list_head *new_e)
{
	new_e->next = old->next;
	new_e->prev = old->prev;
	smp_store_release(&new_e->prev->next, new_e);
	new_e->next->prev = new_e;
	old->prev = (struct list_head *)LIST_POISON2;
}

static inline void
hlist_add_head_rcu(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first;
	n->pprev = &h->first;
	smp_store_release(&h->first, n);
	if (first != NULL)
		first->pprev = &n->next;
}

static inline void
hlist_del_rcu(struct hlist_node *n)
{
	__hlist_del(n);
	n->pprev = (struct hlist_node **)LIST_POISON2;
}

static inline void
hlist_del_init_rcu(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		n->pprev = NULL;
	}
}

#define	list_entry_rcu(ptr, type, member)	container_of(READ_ONCE(ptr), type, member)
#define	list_first_or_null_rcu(ptr, type, member) ({ \
	struct list_head *__ptr = (ptr); \
	struct list_head *__next = READ_ONCE(__ptr->next); \
	__ptr != __next ? list_entry_rcu(__next, type, member) : NULL; })
#define	list_next_or_null_rcu(head, ptr, type, member) ({ \
	struct list_head *__head = (head); struct list_head *__ptr = (ptr); \
	struct list_head *__next = READ_ONCE(__ptr->next); \
	__next != __head ? list_entry_rcu(__next, type, member) : NULL; })
#define	list_for_each_entry_rcu(pos, head, member, cond...) \
	for ((pos) = list_entry_rcu((head)->next, typeof (*(pos)), member); \
	    &(pos)->member != (head); \
	    (pos) = list_entry_rcu((pos)->member.next, typeof (*(pos)), member))
#define	list_for_each_entry_srcu(pos, head, member, c) \
	list_for_each_entry_rcu(pos, head, member)
#define	list_for_each_entry_continue_rcu(pos, head, member) \
	for ((pos) = list_entry_rcu((pos)->member.next, typeof (*(pos)), member); \
	    &(pos)->member != (head); \
	    (pos) = list_entry_rcu((pos)->member.next, typeof (*(pos)), member))
#define	list_for_each_entry_from_rcu(pos, head, member) \
	for (; &(pos)->member != (head); \
	    (pos) = list_entry_rcu((pos)->member.next, typeof (*(pos)), member))
#define	list_entry_lockless(ptr, type, member) \
	container_of((typeof (ptr))READ_ONCE(ptr), type, member)
#define	list_for_each_entry_lockless(pos, head, member) \
	for ((pos) = list_entry_lockless((head)->next, typeof (*(pos)), member); \
	    &(pos)->member != (head); \
	    (pos) = list_entry_lockless((pos)->member.next, typeof (*(pos)), member))
#define	list_for_each_rcu(pos, head) \
	for ((pos) = READ_ONCE((head)->next); (pos) != (head); \
	    (pos) = READ_ONCE((pos)->next))
#define	list_tail_rcu(head)	(*((struct list_head **)(&(head)->prev)))
#define	hlist_first_rcu(head)	(*((struct hlist_node **)(&(head)->first)))
#define	hlist_next_rcu(node)	(*((struct hlist_node **)(&(node)->next)))
#define	hlist_for_each_entry_rcu(pos, head, member, cond...) \
	hlist_for_each_entry(pos, head, member)

#endif
