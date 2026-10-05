/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of <linux/list.h> (doubly linked lists, hlist), <linux/llist.h>
 * and list_sort() (src/lkpi_misc.c): ordering, stability, the rebuilt
 * prev links, and long lists (no recursion).
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/llist.h>
#include "test.h"

struct item {
	struct list_head link;
	struct hlist_node hnode;
	struct llist_node lnode;
	int key;
	int seq;
};

static bool
list_consistent(struct list_head *head, size_t n)
{
	struct list_head *p;
	size_t count = 0;

	for (p = head->next; p != head; p = p->next) {
		if (p->next->prev != p || p->prev->next != p)
			return (false);
		if (++count > n)
			return (false);
	}
	return (count == n && head->prev->next == head);
}

TEST(list_basic)
{
	LIST_HEAD(head);
	LIST_HEAD(other);
	struct item it[6], *pos, *n;
	int i, k;

	CHECK(list_empty(&head));
	for (i = 0; i < 6; i++) {
		it[i].key = i;
		list_add_tail(&it[i].link, &head);
	}
	CHECK(list_consistent(&head, 6));
	CHECK(list_first_entry(&head, struct item, link) == &it[0]);
	CHECK(list_last_entry(&head, struct item, link) == &it[5]);
	CHECK(list_is_first(&it[0].link, &head));
	CHECK(list_is_last(&it[5].link, &head));
	CHECK_EQ(list_count_nodes(&head), 6);
	k = 0;
	list_for_each_entry(pos, &head, link)
		CHECK_EQ(pos->key, k++);
	k = 5;
	list_for_each_entry_reverse(pos, &head, link)
		CHECK_EQ(pos->key, k--);
	/* Delete the odd ones while iterating. */
	list_for_each_entry_safe(pos, n, &head, link) {
		if (pos->key & 1)
			list_del(&pos->link);
	}
	CHECK(list_consistent(&head, 3));
	list_move_tail(&it[0].link, &head);
	CHECK(list_last_entry(&head, struct item, link) == &it[0]);
	list_add(&it[1].link, &other);
	list_add(&it[3].link, &other);
	list_splice_init(&other, &head);
	CHECK(list_empty(&other));
	CHECK(list_consistent(&head, 5));
	CHECK(list_first_entry(&head, struct item, link) == &it[3]);
	list_del_init(&it[3].link);
	CHECK(list_empty(&it[3].link));
	list_replace(&it[1].link, &it[3].link);
	CHECK(list_first_entry(&head, struct item, link) == &it[3]);
	CHECK(list_consistent(&head, 4));
	list_rotate_left(&head);
	CHECK(list_last_entry(&head, struct item, link) == &it[3]);
	CHECK(list_consistent(&head, 4));
}

static int
cmp_key(void *priv, const struct list_head *a, const struct list_head *b)
{
	const struct item *x = list_entry(a, struct item, link);
	const struct item *y = list_entry(b, struct item, link);

	(*(int *)priv)++;
	return (x->key > y->key);	/* Linux allows a bool-like result */
}

static bool
sorted_stable(struct list_head *head)
{
	struct item *pos, *prev = NULL;

	list_for_each_entry(pos, head, link) {
		if (prev != NULL && (prev->key > pos->key ||
		    (prev->key == pos->key && prev->seq > pos->seq)))
			return (false);
		prev = pos;
	}
	return (true);
}

TEST(list_sort)
{
	static struct item items[100000];
	unsigned long long seed = 99;
	size_t sizes[] = { 0, 1, 2, 3, 7, 64, 1000, 100000 };
	size_t s, i;
	int calls;

	for (s = 0; s < ARRAY_SIZE(sizes); s++) {
		LIST_HEAD(head);
		size_t n = sizes[s];

		for (i = 0; i < n; i++) {
			/* Few distinct keys: many ties to test stability. */
			items[i].key = (int)(t_rand(&seed) % 17);
			items[i].seq = (int)i;
			list_add_tail(&items[i].link, &head);
		}
		calls = 0;
		list_sort(&calls, &head, cmp_key);
		CHECK(list_consistent(&head, n));
		CHECK(sorted_stable(&head));
		if (n == 100000) {
			/* O(n log n): at most n * ceil(log2 n) comparisons. */
			CHECK(calls <= 100000 * 17);
		}
	}
	/* Already sorted and reverse-sorted input. */
	{
		LIST_HEAD(head);

		for (i = 0; i < 1000; i++) {
			items[i].key = (int)(1000 - i);
			items[i].seq = (int)i;
			list_add_tail(&items[i].link, &head);
		}
		list_sort(&calls, &head, cmp_key);
		CHECK(list_consistent(&head, 1000));
		CHECK(sorted_stable(&head));
		list_sort(&calls, &head, cmp_key);
		CHECK(sorted_stable(&head));
		CHECK(list_first_entry(&head, struct item, link)->key == 1);
	}
}

TEST(hlist)
{
	struct hlist_head h = HLIST_HEAD_INIT;
	struct item it[4], *pos;
	struct hlist_node *tmp;
	int n = 0;

	CHECK(hlist_empty(&h));
	for (n = 0; n < 4; n++) {
		it[n].key = n;
		INIT_HLIST_NODE(&it[n].hnode);
		CHECK(hlist_unhashed(&it[n].hnode));
		hlist_add_head(&it[n].hnode, &h);
	}
	n = 3;
	hlist_for_each_entry(pos, &h, hnode)
		CHECK_EQ(pos->key, n--);
	hlist_for_each_entry_safe(pos, tmp, &h, hnode) {
		if (pos->key == 2)
			hlist_del_init(&pos->hnode);
	}
	CHECK(hlist_unhashed(&it[2].hnode));
	hlist_add_behind(&it[2].hnode, &it[0].hnode);
	{
		int want[] = { 3, 1, 0, 2 };

		n = 0;
		hlist_for_each_entry(pos, &h, hnode)
			CHECK_EQ(pos->key, want[n++]);
		CHECK_EQ(n, 4);
	}
	hlist_del(&it[2].hnode);
	hlist_add_before(&it[2].hnode, &it[1].hnode);
	n = 3;
	hlist_for_each_entry(pos, &h, hnode)
		CHECK_EQ(pos->key, n--);
	CHECK_EQ(n, -1);
	hlist_del(&it[3].hnode);
	CHECK(h.first == &it[2].hnode);
}

TEST(llist)
{
	LLIST_HEAD(h);
	struct item it[5], *pos;
	struct llist_node *all;
	int n;

	CHECK(llist_empty(&h));
	for (n = 0; n < 5; n++) {
		it[n].key = n;
		(void) llist_add(&it[n].lnode, &h);
	}
	CHECK(!llist_empty(&h));
	CHECK(llist_del_first(&h) == &it[4].lnode);
	all = llist_del_all(&h);
	CHECK(llist_empty(&h));
	n = 3;
	llist_for_each_entry(pos, all, lnode)
		CHECK_EQ(pos->key, n--);
	all = llist_reverse_order(all);
	n = 0;
	llist_for_each_entry(pos, all, lnode)
		CHECK_EQ(pos->key, n++);
	CHECK_EQ(n, 4);
}
