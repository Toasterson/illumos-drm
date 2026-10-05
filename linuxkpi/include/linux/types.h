/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's basic kernel types (include/linux/types.h and
 * include/uapi/asm-generic/int-ll64.h; the uapi part is GPL-2.0 WITH
 * Linux-syscall-note, the rest GPL-2.0; reimplemented). The DRM facade's
 * base types (struct list_head, atomic_t, refcount_t, struct kref, struct
 * rcu_head, ktime_t, gfp_t) are used as they are. u64 and __u64 are
 * uint64_t (unsigned long on illumos' LP64), not Linux's unsigned long
 * long: the facade's interfaces are uint64_t, and a driver passes pointers
 * to its u64 fields to them. Printing a u64 with %llx is only a format
 * warning.
 */
#ifndef _LINUX_TYPES_H
#define	_LINUX_TYPES_H

#include <linux/compiler.h>
#include <linux/container_of.h>
#include <drm/drm_facade_types.h>

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int8_t		s8;
typedef int16_t		s16;
typedef int32_t		s32;
typedef int64_t		s64;
typedef uint8_t		__u8;
typedef uint16_t	__u16;
typedef uint32_t	__u32;
typedef uint64_t	__u64;
typedef int8_t		__s8;
typedef int16_t		__s16;
typedef int32_t		__s32;
typedef int64_t		__s64;
typedef __u16		__le16;
typedef __u16		__be16;
typedef __u32		__le32;
typedef __u32		__be32;
typedef __u64		__le64;
typedef __u64		__be64;
typedef __u16		__sum16;
typedef __u32		__wsum;
typedef __u64		__aligned_u64 __attribute__((__aligned__(8)));
typedef __s64		__aligned_s64 __attribute__((__aligned__(8)));
typedef __u64		__aligned_be64 __attribute__((__aligned__(8)));
typedef __u64		__aligned_le64 __attribute__((__aligned__(8)));
typedef unsigned __int128	u128;
typedef __int128	s128;

typedef unsigned char	uchar;
typedef unsigned short	ushort;
typedef unsigned int	uint;
typedef unsigned long	ulong;
typedef unsigned char	unchar;

typedef u64		dma_addr_t;
typedef u64		phys_addr_t;
typedef phys_addr_t	resource_size_t;
typedef long long	loff_t;
typedef unsigned long	pgoff_t;
typedef unsigned long	irq_hw_number_t;
typedef unsigned short	umode_t;
typedef unsigned int	fmode_t;
typedef unsigned int	slab_flags_t;
typedef unsigned int	vm_fault_t;
typedef int		pci_power_t;
typedef unsigned int	__poll_t;
typedef u64		sector_t;
typedef int		rwf_t;
typedef size_t		__kernel_size_t;
typedef ssize_t		__kernel_ssize_t;
typedef long		__kernel_long_t;
typedef unsigned long	__kernel_ulong_t;
typedef long long	__kernel_loff_t;
typedef int		__kernel_pid_t;
typedef int		__kernel_timer_t;
typedef int		__kernel_clockid_t;
typedef long long	time64_t;
typedef unsigned long long timeu64_t;
typedef int		clockid_t_lx;

typedef struct {
	volatile long long counter;
} atomic64_t;

typedef struct {
	volatile long counter;
} atomic_long_t;

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#define	callback_head	rcu_head
typedef void (*rcu_callback_t)(struct rcu_head *head);
typedef void (*call_rcu_func_t)(struct rcu_head *head, rcu_callback_t func);

struct ustat;

#define	DECLARE_BITMAP(name, bits) \
	unsigned long name[((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG]

#endif
