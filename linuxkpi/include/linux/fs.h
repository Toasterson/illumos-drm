/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's files, inodes and address spaces, reduced to the layer's
 * shmem files (include/linux/fs.h, GPL-2.0; independent design).
 * struct file is struct lx_file (the prologue renames the tag; illumos
 * has its own). A layer file is a reference-counted object with an inode
 * and an address space; the only files the layer creates are shmem files
 * (<linux/shmem_fs.h>) and the dma-buf/sync-file wrappers of the DRM
 * core. User file descriptors stay the DRM core's (illumos vnodes).
 */
#ifndef _LINUX_FS_H
#define	_LINUX_FS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/uidgid.h>
#include <linux/module.h>

struct lx_file;
struct inode;
struct poll_table_struct;
struct seq_file;
struct kiocb;
struct iov_iter;
struct dentry;
struct super_block;
struct vfsmount;
struct writeback_control;
struct lkpi_shmem;

struct address_space_operations {
	int (*writepage)(struct lx_page *page, struct writeback_control *wbc);
	bool (*dirty_folio)(struct address_space *m, struct folio *f);
};

struct address_space {
	struct inode *host;
	gfp_t gfp_mask;
	unsigned long flags;
	pgoff_t nrpages;
	const struct address_space_operations *a_ops;
	void *private_data;
	struct lkpi_shmem *lkpi_shmem;
};

#define	AS_UNEVICTABLE	3

struct inode {
	umode_t i_mode;
	unsigned int i_flags;
	loff_t i_size;
	unsigned long i_ino;
	atomic_t i_count;
	struct address_space *i_mapping;
	struct address_space i_data;
	void *i_private;
};

struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

struct file_operations {
	struct lx_module *owner;
	loff_t (*llseek)(struct lx_file *, loff_t, int);
	ssize_t (*read)(struct lx_file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct lx_file *, const char __user *, size_t, loff_t *);
	__poll_t (*poll)(struct lx_file *, struct poll_table_struct *);
	long (*unlocked_ioctl)(struct lx_file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct lx_file *, unsigned int, unsigned long);
	int (*mmap)(struct lx_file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct lx_file *);
	int (*flush)(struct lx_file *, void *id);
	int (*release)(struct inode *, struct lx_file *);
	int (*fasync)(int, struct lx_file *, int);
	void (*show_fdinfo)(struct seq_file *m, struct lx_file *f);
	unsigned long (*get_unmapped_area)(struct lx_file *, unsigned long,
	    unsigned long, unsigned long, unsigned long);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	unsigned int fop_flags;
};

/* Synchronous I/O control blocks and user-buffer iterators (shmem pwrite). */
struct kiocb {
	struct lx_file *ki_filp;
	loff_t ki_pos;
	int ki_flags;
};

#define	ITER_SOURCE	1
#define	ITER_DEST	0
#define	ITER_UBUF	0

struct iov_iter {
	u8 iter_type;
	bool data_source;
	size_t iov_offset;
	size_t count;
	void __user *ubuf;
};

static inline void
iov_iter_ubuf(struct iov_iter *i, unsigned int direction, void __user *buf,
    size_t count)
{
	i->iter_type = ITER_UBUF;
	i->data_source = (direction == ITER_SOURCE);
	i->iov_offset = 0;
	i->count = count;
	i->ubuf = buf;
}

#define	iov_iter_count(i)	((i)->count)
#define	MAX_RW_COUNT		(INT_MAX & PAGE_MASK)
#define	O_LARGEFILE		0x2000
#define	force_o_largefile()	true

/* Writeback: illumos kernel memory is not paged out; nothing is written back. */
enum writeback_sync_modes {
	WB_SYNC_NONE,
	WB_SYNC_ALL
};

struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned int for_kupdate:1;
	unsigned int for_background:1;
	unsigned int for_reclaim:1;
	unsigned int range_cyclic:1;
};

static inline struct folio *
writeback_iter(struct address_space *mapping, struct writeback_control *wbc,
    struct folio *folio, int *error)
{
	(void) mapping; (void) wbc; (void) folio;
	*error = 0;
	return (NULL);
}

#define	folio_mapped(f)				((void)(f), false)
#define	folio_redirty_for_writepage(wbc, f)	((void)(wbc), (void)(f), false)

struct lx_file {
	atomic_long_t f_count;
	struct inode *f_inode;
	struct address_space *f_mapping;
	const struct file_operations *f_op;
	void *private_data;
	fmode_t f_mode;
	unsigned int f_flags;
	loff_t f_pos;
	struct path f_path;
	struct mutex f_pos_lock;
};

#define	O_ACCMODE	3
#ifndef	O_RDONLY
#define	O_RDONLY	0
#define	O_WRONLY	1
#define	O_RDWR		2
#endif
#ifndef	O_NONBLOCK
#define	O_NONBLOCK	0x80
#endif
#ifndef	O_CLOEXEC
#define	O_CLOEXEC	0x800000
#endif
#define	FMODE_READ		0x1
#define	FMODE_WRITE		0x2
#define	FMODE_LSEEK		0x4
#define	FMODE_PREAD		0x8
#define	FMODE_PWRITE		0x10
#define	FMODE_UNSIGNED_OFFSET	0x2000
#define	FOP_UNSIGNED_OFFSET	0x20
#define	MAX_LFS_FILESIZE	((loff_t)LLONG_MAX)
#define	SEEK_SET		0
#define	SEEK_CUR		1
#define	SEEK_END		2

extern void fput(struct lx_file *file);
extern ssize_t kernel_write(struct lx_file *file, const void *buf, size_t count,
    loff_t *pos);
extern ssize_t kernel_read(struct lx_file *file, void *buf, size_t count,
    loff_t *pos);

static inline void
init_sync_kiocb(struct kiocb *kiocb, struct lx_file *filp)
{
	kiocb->ki_filp = filp;
	kiocb->ki_pos = 0;
	kiocb->ki_flags = 0;
}
extern struct lx_file *get_file(struct lx_file *file);
extern loff_t noop_llseek(struct lx_file *file, loff_t offset, int whence);
extern loff_t default_llseek(struct lx_file *file, loff_t offset, int whence);
extern int simple_open(struct inode *inode, struct lx_file *file);
extern ssize_t simple_read_from_buffer(void __user *to, size_t count,
    loff_t *ppos, const void *from, size_t available);
extern ssize_t simple_write_to_buffer(void *to, size_t available,
    loff_t *ppos, const void __user *from, size_t count);
extern int nonseekable_open(struct inode *inode, struct lx_file *filp);
extern int stream_open(struct inode *inode, struct lx_file *filp);

#define	file_inode(f)		((f)->f_inode)
#define	file_count(f)		atomic_long_read(&(f)->f_count)
#define	get_file_rcu(f)		get_file(f)
extern struct lx_file *get_file_active(struct lx_file **f);
extern int vfs_mmap(struct lx_file *file, struct vm_area_struct *vma);
#define	i_size_read(inode)	((inode)->i_size)
#define	i_size_write(inode, s)	((inode)->i_size = (s))
#define	iput(inode)		((void)(inode))
#define	ihold(inode)		((void)(inode))
#define	no_llseek		noop_llseek
#define	compat_ptr_ioctl	NULL
#define	inode_lock(i)		((void)(i))
#define	inode_unlock(i)		((void)(i))

static inline void
mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{
	m->gfp_mask = mask;
}

static inline gfp_t
mapping_gfp_mask(struct address_space *m)
{
	return (m->gfp_mask);
}

static inline gfp_t
mapping_gfp_constraint(struct address_space *m, gfp_t gfp)
{
	return (m->gfp_mask & gfp);
}

static inline void
mapping_set_unevictable(struct address_space *m)
{
	set_bit(AS_UNEVICTABLE, &m->flags);
}

static inline void
mapping_clear_unevictable(struct address_space *m)
{
	clear_bit(AS_UNEVICTABLE, &m->flags);
}

static inline bool
mapping_unevictable(struct address_space *m)
{
	return (test_bit(AS_UNEVICTABLE, &m->flags));
}

#define	mapping_mapped(m)	false

/* simple_attr files (debugfs only, which illumos lacks): the fops exist, nothing opens them. */
#define	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt) \
	static const struct file_operations fops __maybe_unused = { .owner = NULL }
#define	DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(fops, get, set, fmt) \
	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt)
#define	DEFINE_SIMPLE_ATTRIBUTE(fops, get, set, fmt) \
	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt)


#endif
