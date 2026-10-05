/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's files and shmem files (the interfaces of Linux's
 * <linux/fs.h>, <linux/shmem_fs.h> and <linux/pagemap.h>; independent
 * implementation).
 *
 * A layer file is reference counted (get_file()/fput()); its creator's
 * f_op->release() runs at the last fput() and frees it. The only files
 * created here are shmem files: a struct lkpi_shmem_file holds the file,
 * its inode and the shmem state the address space points to.
 *
 * A shmem file's memory is one ddi_umem_alloc(9F) allocation of the
 * page-rounded file size (zero-filled, locked), the "backing", made at the
 * first page read, write or lkpi_shmem_umem(). Page descriptors are
 * created on first read and kept in the file's page array; the file holds
 * one reference on each, the reader gets another. Each descriptor holds a
 * reference on the backing, as does the file while the backing is
 * current, so memory a driver still holds a page of (a DMA mapping, a
 * vmap()) stays valid after truncation; the backing is freed with its
 * last reference.
 *
 * shmem_truncate_range() of the whole file drops the file's pages and
 * backing (the next read allocates a new, zero-filled backing); a partial
 * truncation zeroes the range. Once the DRM core has the backing's cookie
 * (lkpi_shmem_umem(), for user mappings), a whole-file truncation zeroes
 * the memory instead of freeing it: user mappings of the cookie may still
 * exist, and the backing is then freed when the file is released (the GEM
 * object, and with it every user mapping, is gone by then). Linux would
 * make such a mapping fault; here it reads zeros.
 *
 * There is no swap: shmem_writeout() leaves the folio in place and
 * returns 0, invalidate_mapping_pages() keeps the pages (they are the
 * file's memory, not a cache of it).
 */

#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

/* The backing allocation of a shmem file. */
struct lkpi_shmem_mem {
	struct lkpi_page_owner	sm_owner;	/* first: owner of the pages */
	ddi_umem_cookie_t	sm_cookie;
	caddr_t			sm_kva;
	size_t			sm_size;
	volatile uint32_t	sm_refs;	/* the file's + one per page */
};

struct lkpi_shmem {
	kmutex_t		ls_lock;
	size_t			ls_size;	/* page-rounded */
	pgoff_t			ls_npages;
	struct lkpi_shmem_mem	*ls_mem;	/* NULL until first use */
	struct lx_page		**ls_pages;	/* ls_npages slots, with ls_mem */
	bool			ls_exported;	/* the cookie was handed out */
};

struct lkpi_shmem_file {
	struct lx_file		sf_file;
	struct inode		sf_inode;
	struct lkpi_shmem	sf_shmem;
};

static volatile unsigned long lkpi_shmem_ino;

static void
lkpi_shmem_mem_rele(struct lkpi_shmem_mem *m)
{
	if (__atomic_sub_fetch(&m->sm_refs, 1, __ATOMIC_SEQ_CST) != 0)
		return;
	ddi_umem_free(m->sm_cookie);
	kmem_free(m, sizeof (*m));
}

/* A page's last reference: the descriptor goes, the backing may follow. */
static void
lkpi_shmem_page_release(struct lkpi_page_owner *owner, struct lx_page *page)
{
	struct lkpi_shmem_mem *m = (struct lkpi_shmem_mem *)owner;

	lkpi_page_destroy(page);
	lkpi_shmem_mem_rele(m);
}

/* Makes sure the file has its backing; ls_lock held. 0 or -errno. */
static int
lkpi_shmem_mem_get(struct lkpi_shmem *ls, bool sleep)
{
	struct lkpi_shmem_mem *m;
	int kmflag = sleep ? KM_SLEEP : KM_NOSLEEP;

	ASSERT(MUTEX_HELD(&ls->ls_lock));
	if (ls->ls_mem != NULL)
		return (0);
	if (ls->ls_size == 0)
		return (-EINVAL);
	if ((m = kmem_zalloc(sizeof (*m), kmflag)) == NULL)
		return (-ENOMEM);
	m->sm_kva = ddi_umem_alloc(ls->ls_size,
	    sleep ? DDI_UMEM_SLEEP : DDI_UMEM_NOSLEEP, &m->sm_cookie);
	if (m->sm_kva == NULL) {
		kmem_free(m, sizeof (*m));
		return (-ENOMEM);
	}
	ls->ls_pages = kmem_zalloc(ls->ls_npages * sizeof (struct lx_page *),
	    kmflag);
	if (ls->ls_pages == NULL) {
		ddi_umem_free(m->sm_cookie);
		kmem_free(m, sizeof (*m));
		return (-ENOMEM);
	}
	m->sm_owner.lpo_release = lkpi_shmem_page_release;
	m->sm_owner.lpo_name = "shmem";
	m->sm_size = ls->ls_size;
	m->sm_refs = 1;
	ls->ls_mem = m;
	return (0);
}

/* Drops the file's pages and backing; ls_lock held. */
static void
lkpi_shmem_drop(struct lkpi_shmem *ls)
{
	struct lkpi_shmem_mem *m = ls->ls_mem;
	struct lx_page *page;
	pgoff_t i;

	ASSERT(MUTEX_HELD(&ls->ls_lock));
	if (m == NULL)
		return;
	for (i = 0; i < ls->ls_npages; i++) {
		if ((page = ls->ls_pages[i]) == NULL)
			continue;
		ls->ls_pages[i] = NULL;
		page->mapping = NULL;
		put_page(page);
	}
	kmem_free(ls->ls_pages, ls->ls_npages * sizeof (struct lx_page *));
	ls->ls_pages = NULL;
	ls->ls_mem = NULL;
	ls->ls_exported = false;
	lkpi_shmem_mem_rele(m);
}

static struct lkpi_shmem *
lkpi_shmem_of_file(struct lx_file *file)
{
	if (file == NULL || file->f_mapping == NULL)
		return (NULL);
	return (file->f_mapping->lkpi_shmem);
}

/*
 * Copies between the file and a kernel buffer (kbuf) or a user address
 * (ubuf, through the DRM core's ioctl copy). Returns the bytes copied,
 * 0 at or past the end for a read, or -errno.
 */
static ssize_t
lkpi_shmem_rw(struct lkpi_shmem *ls, loff_t pos, void *kbuf, uint64_t ubuf,
    size_t len, bool write)
{
	size_t done = 0, n;
	caddr_t kva;
	int err;

	if (pos < 0)
		return (-EINVAL);
	if ((uint64_t)pos >= ls->ls_size)
		return (write && len != 0 ? -ENOSPC : 0);
	len = MIN(len, ls->ls_size - (size_t)pos);
	if (len == 0)
		return (0);

	mutex_enter(&ls->ls_lock);
	if ((err = lkpi_shmem_mem_get(ls, true)) != 0) {
		mutex_exit(&ls->ls_lock);
		return (err);
	}
	kva = ls->ls_mem->sm_kva + pos;
	while (done < len) {
		/* Page by page, so a fault still reports the bytes copied. */
		n = MIN(len - done, PAGE_SIZE - ((uintptr_t)(kva + done) &
		    (PAGE_SIZE - 1)));
		if (kbuf != NULL) {
			if (write)
				bcopy((char *)kbuf + done, kva + done, n);
			else
				bcopy(kva + done, (char *)kbuf + done, n);
		} else {
			err = write ?
			    drm_copy_from_user(kva + done, ubuf + done, n) :
			    drm_copy_to_user(ubuf + done, kva + done, n);
			if (err != 0)
				break;
		}
		done += n;
	}
	mutex_exit(&ls->ls_lock);
	return (done == 0 && len != 0 ? -EFAULT : (ssize_t)done);
}

static ssize_t
lkpi_shmem_iter(struct kiocb *kiocb, struct iov_iter *iter, bool write)
{
	struct lkpi_shmem *ls = lkpi_shmem_of_file(kiocb->ki_filp);
	ssize_t n;

	if (ls == NULL || iter->data_source != write)
		return (-EINVAL);
	n = lkpi_shmem_rw(ls, kiocb->ki_pos, NULL,
	    (uint64_t)(uintptr_t)iter->ubuf + iter->iov_offset, iter->count,
	    write);
	if (n > 0) {
		kiocb->ki_pos += n;
		iter->iov_offset += n;
		iter->count -= n;
	}
	return (n);
}

static ssize_t
lkpi_shmem_write_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	return (lkpi_shmem_iter(kiocb, iter, true));
}

static ssize_t
lkpi_shmem_read_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	return (lkpi_shmem_iter(kiocb, iter, false));
}

static int
lkpi_shmem_release(struct inode *inode, struct lx_file *file)
{
	struct lkpi_shmem_file *sf = container_of(file,
	    struct lkpi_shmem_file, sf_file);
	struct lkpi_shmem *ls = &sf->sf_shmem;

	mutex_enter(&ls->ls_lock);
	lkpi_shmem_drop(ls);
	mutex_exit(&ls->ls_lock);
	(mutex_destroy)(&ls->ls_lock);
	mutex_destroy(&file->f_pos_lock);
	kmem_free(sf, sizeof (*sf));
	return (0);
}

static const struct file_operations lkpi_shmem_fops = {
	.llseek = default_llseek,
	.read_iter = lkpi_shmem_read_iter,
	.write_iter = lkpi_shmem_write_iter,
	.release = lkpi_shmem_release
};

struct lx_file *
shmem_file_setup(const char *name, loff_t size, vma_flags_t flags)
{
	struct lkpi_shmem_file *sf;
	struct lx_file *file;
	struct inode *inode;
	struct lkpi_shmem *ls;

	if (size < 0 || size > MAX_LFS_FILESIZE ||
	    (uint64_t)size > (uint64_t)SIZE_MAX - PAGE_SIZE)
		return (ERR_PTR(-EINVAL));

	sf = kmem_zalloc(sizeof (*sf), KM_SLEEP);
	file = &sf->sf_file;
	inode = &sf->sf_inode;
	ls = &sf->sf_shmem;

	(mutex_init)(&ls->ls_lock, NULL, MUTEX_DRIVER, NULL);
	ls->ls_size = P2ROUNDUP((size_t)size, PAGE_SIZE);
	ls->ls_npages = ls->ls_size >> PAGE_SHIFT;

	inode->i_mode = S_IFREG | 0600;
	inode->i_size = size;
	inode->i_ino = __atomic_add_fetch(&lkpi_shmem_ino, 1,
	    __ATOMIC_RELAXED);
	atomic_set(&inode->i_count, 1);
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	inode->i_data.gfp_mask = GFP_HIGHUSER_MOVABLE;
	inode->i_data.lkpi_shmem = ls;

	atomic_long_set(&file->f_count, 1);
	file->f_inode = inode;
	file->f_mapping = inode->i_mapping;
	file->f_op = &lkpi_shmem_fops;
	file->f_mode = FMODE_READ | FMODE_WRITE | FMODE_LSEEK | FMODE_PREAD |
	    FMODE_PWRITE;
	file->f_flags = O_RDWR;
	mutex_init(&file->f_pos_lock);
	return (file);
}

/* Mount points (huge pages) do not exist here; the file is the same. */
struct lx_file *
shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
    loff_t size, vma_flags_t flags)
{
	return (shmem_file_setup(name, size, flags));
}

struct folio *
shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	struct lkpi_shmem *ls;
	struct lkpi_shmem_mem *m;
	struct lx_page *page;
	bool sleep = gfpflags_allow_blocking(gfp);
	int err;

	if (mapping == NULL || (ls = mapping->lkpi_shmem) == NULL)
		return (ERR_PTR(-EINVAL));

	mutex_enter(&ls->ls_lock);
	if (index >= ls->ls_npages) {
		mutex_exit(&ls->ls_lock);
		return (ERR_PTR(-EINVAL));
	}
	if ((err = lkpi_shmem_mem_get(ls, sleep)) != 0) {
		mutex_exit(&ls->ls_lock);
		return (ERR_PTR(err));
	}
	m = ls->ls_mem;
	if ((page = ls->ls_pages[index]) == NULL) {
		page = lkpi_page_create(m->sm_kva + LKPI_PTOB(index), &m->sm_owner,
		    sleep ? KM_SLEEP : KM_NOSLEEP);
		if (page == NULL) {
			mutex_exit(&ls->ls_lock);
			return (ERR_PTR(-ENOMEM));
		}
		page->mapping = mapping;
		page->index = index;
		__atomic_add_fetch(&m->sm_refs, 1, __ATOMIC_SEQ_CST);
		ls->ls_pages[index] = page;
	}
	get_page(page);
	mutex_exit(&ls->ls_lock);
	return (page_folio(page));
}

struct folio *
lkpi_shmem_read_folio(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	return (shmem_read_folio_gfp(mapping, index, gfp));
}

struct lx_page *
shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index,
    gfp_t gfp)
{
	struct folio *folio = shmem_read_folio_gfp(mapping, index, gfp);

	if (IS_ERR(folio))
		return (ERR_CAST(folio));
	return (&folio->page);
}

/* lend is the last byte, inclusive; (loff_t)-1 means the end of the file. */
void
shmem_truncate_range(struct inode *inode, loff_t lstart, loff_t lend)
{
	struct lkpi_shmem *ls;
	size_t start, end;

	if (inode == NULL || inode->i_mapping == NULL ||
	    (ls = inode->i_mapping->lkpi_shmem) == NULL)
		return;
	mutex_enter(&ls->ls_lock);
	start = lstart < 0 ? 0 : (size_t)MIN((uint64_t)lstart, ls->ls_size);
	end = (lend < 0 || (uint64_t)lend >= ls->ls_size) ? ls->ls_size :
	    (size_t)lend + 1;
	if (ls->ls_mem == NULL || start >= end) {
		mutex_exit(&ls->ls_lock);
		return;
	}
	if (start == 0 && end == ls->ls_size && !ls->ls_exported)
		lkpi_shmem_drop(ls);
	else
		bzero(ls->ls_mem->sm_kva + start, end - start);
	mutex_exit(&ls->ls_lock);
}

/* No swap: the folio stays where it is. */
int
shmem_writeout(struct folio *folio, struct swap_iocb **plug,
    struct list_head *folio_list)
{
	return (0);
}

void
invalidate_mapping_pages(struct address_space *mapping, pgoff_t start,
    pgoff_t end)
{
}

ddi_umem_cookie_t
lkpi_shmem_umem(struct lx_file *file, void **kvap, size_t *sizep)
{
	struct lkpi_shmem *ls = lkpi_shmem_of_file(file);
	ddi_umem_cookie_t cookie = NULL;

	if (ls == NULL)
		return (NULL);
	mutex_enter(&ls->ls_lock);
	if (lkpi_shmem_mem_get(ls, true) == 0) {
		ls->ls_exported = true;
		cookie = ls->ls_mem->sm_cookie;
		if (kvap != NULL)
			*kvap = ls->ls_mem->sm_kva;
		if (sizep != NULL)
			*sizep = ls->ls_mem->sm_size;
	}
	mutex_exit(&ls->ls_lock);
	return (cookie);
}

/* --- files ------------------------------------------------------------- */

void
fput(struct lx_file *file)
{
	if (file == NULL || !atomic_long_dec_and_test(&file->f_count))
		return;
	if (file->f_op == NULL || file->f_op->release == NULL) {
		WARN(1, "fput: file %p has no release", file);
		return;
	}
	(void) file->f_op->release(file->f_inode, file);
}

struct lx_file *
get_file(struct lx_file *file)
{
	atomic_long_inc(&file->f_count);
	return (file);
}

/*
 * A reference to *f unless it is being released. Layer files are not freed
 * after a grace period, so the caller must keep *f valid by other means
 * (as i915 does with its own locks); RCU only orders the reads.
 */
struct lx_file *
get_file_active(struct lx_file **f)
{
	struct lx_file *file;

	rcu_read_lock();
	for (;;) {
		file = READ_ONCE(*f);
		if (file == NULL || !atomic_long_inc_not_zero(&file->f_count)) {
			file = NULL;
			break;
		}
		if (READ_ONCE(*f) == file)
			break;
		fput(file);
	}
	rcu_read_unlock();
	return (file);
}

ssize_t
kernel_write(struct lx_file *file, const void *buf, size_t count, loff_t *pos)
{
	struct lkpi_shmem *ls = lkpi_shmem_of_file(file);
	ssize_t n;

	/* Only shmem files take kernel buffers. */
	if (ls == NULL || pos == NULL)
		return (-EINVAL);
	n = lkpi_shmem_rw(ls, *pos, (void *)buf, 0, count, true);
	if (n > 0)
		*pos += n;
	return (n);
}

ssize_t
kernel_read(struct lx_file *file, void *buf, size_t count, loff_t *pos)
{
	struct lkpi_shmem *ls = lkpi_shmem_of_file(file);
	ssize_t n;

	if (ls == NULL || pos == NULL)
		return (-EINVAL);
	n = lkpi_shmem_rw(ls, *pos, buf, 0, count, false);
	if (n > 0)
		*pos += n;
	return (n);
}

loff_t
noop_llseek(struct lx_file *file, loff_t offset, int whence)
{
	return (file->f_pos);
}

loff_t
default_llseek(struct lx_file *file, loff_t offset, int whence)
{
	loff_t pos;

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	case SEEK_END:
		pos = (file->f_inode != NULL ? file->f_inode->i_size : 0) +
		    offset;
		break;
	default:
		return (-EINVAL);
	}
	if (pos < 0)
		return (-EINVAL);
	file->f_pos = pos;
	return (pos);
}

int
simple_open(struct inode *inode, struct lx_file *file)
{
	if (inode != NULL && inode->i_private != NULL)
		file->private_data = inode->i_private;
	return (0);
}

int
nonseekable_open(struct inode *inode, struct lx_file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return (0);
}

int
stream_open(struct inode *inode, struct lx_file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return (0);
}
