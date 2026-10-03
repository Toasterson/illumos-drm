/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's folio batches (include/linux/folio_batch.h, GPL-2.0;
 * reimplemented): a small array of folio references released together.
 */
#ifndef _LINUX_FOLIO_BATCH_H
#define	_LINUX_FOLIO_BATCH_H

#include <linux/mm.h>

#define	PAGEVEC_SIZE	31

struct folio_batch {
	unsigned char nr;
	unsigned char i;
	bool percpu_pvec_drained;
	struct folio *folios[PAGEVEC_SIZE];
};

static inline void
folio_batch_init(struct folio_batch *fbatch)
{
	fbatch->nr = 0;
	fbatch->i = 0;
	fbatch->percpu_pvec_drained = false;
}

static inline void
folio_batch_reinit(struct folio_batch *fbatch)
{
	fbatch->nr = 0;
	fbatch->i = 0;
}

static inline unsigned int
folio_batch_count(struct folio_batch *fbatch)
{
	return (fbatch->nr);
}

static inline unsigned int
folio_batch_space(struct folio_batch *fbatch)
{
	return (PAGEVEC_SIZE - fbatch->nr);
}

static inline unsigned int
folio_batch_add(struct folio_batch *fbatch, struct folio *folio)
{
	fbatch->folios[fbatch->nr++] = folio;
	return (folio_batch_space(fbatch));
}

static inline struct folio *
folio_batch_next(struct folio_batch *fbatch)
{
	if (fbatch->i == fbatch->nr)
		return (NULL);
	return (fbatch->folios[fbatch->i++]);
}

static inline void
folio_batch_release(struct folio_batch *fbatch)
{
	unsigned int n;

	for (n = 0; n < fbatch->nr; n++)
		folio_put(fbatch->folios[n]);
	folio_batch_reinit(fbatch);
}

#define	folios_put(fb)	folio_batch_release(fb)

#endif
