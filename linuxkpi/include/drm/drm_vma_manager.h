/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's mmap offset manager (include/drm/drm_vma_manager.h, MIT;
 * reimplemented). The facade defines struct drm_vma_offset_node (inside
 * struct drm_gem_object) and gives each object one offset itself
 * (drm_gem_create_mmap_offset()); the offsets are the core's, served by
 * its devmap(9E) entry point. Linux's manager functions for additional
 * offsets per object (i915's mmap-offset scheme) are therefore not
 * supported yet: drm_vma_offset_add() fails with -ENODEV, lookups find
 * nothing and access grants are refused (src/lkpi_drm.c); see the
 * i915 mmap-offset facade extension (open issue).
 */
#ifndef _LKPI_DRM_VMA_MANAGER_H
#define	_LKPI_DRM_VMA_MANAGER_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <drm/drm_facade_types.h>

struct drm_file;
struct drm_vma_offset_manager;
struct drm_vma_offset_node;

#define	DRM_FILE_PAGE_OFFSET_START	((0xFFFFFFFFUL >> PAGE_SHIFT) + 1)
#define	DRM_FILE_PAGE_OFFSET_SIZE	((0xFFFFFFFFUL >> PAGE_SHIFT) * 256)

extern int drm_vma_offset_add(struct drm_vma_offset_manager *mgr,
    struct drm_vma_offset_node *node, unsigned long pages);
extern void drm_vma_offset_remove(struct drm_vma_offset_manager *mgr,
    struct drm_vma_offset_node *node);
extern struct drm_vma_offset_node *drm_vma_offset_lookup_locked(
    struct drm_vma_offset_manager *mgr, unsigned long start,
    unsigned long pages);
extern int drm_vma_node_allow(struct drm_vma_offset_node *node,
    struct drm_file *tag);
extern int drm_vma_node_allow_once(struct drm_vma_offset_node *node,
    struct drm_file *tag);
extern void drm_vma_node_revoke(struct drm_vma_offset_node *node,
    struct drm_file *tag);
extern bool drm_vma_node_is_allowed(struct drm_vma_offset_node *node,
    struct drm_file *tag);
extern void drm_vma_offset_lock_lookup(struct drm_vma_offset_manager *mgr);
extern void drm_vma_offset_unlock_lookup(struct drm_vma_offset_manager *mgr);

#endif
