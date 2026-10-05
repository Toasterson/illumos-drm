/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The dma-fence tracepoints, empty (see <linux/tracepoint.h>).
 */
#ifndef _TRACE_EVENTS_DMA_FENCE_H
#define	_TRACE_EVENTS_DMA_FENCE_H
#include <linux/tracepoint.h>
struct dma_fence;
DEFINE_EVENT(dma_fence, dma_fence_emit, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_init, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_destroy, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_enable_signal, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_signaled, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_wait_start, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
DEFINE_EVENT(dma_fence, dma_fence_wait_end, TP_PROTO(struct dma_fence *fence), TP_ARGS(fence))
#endif
