/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's DMA directions. */
#ifndef _LINUX_DMA_DIRECTION_H
#define	_LINUX_DMA_DIRECTION_H
enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3
};
#endif
