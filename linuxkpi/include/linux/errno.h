/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux error numbers. The names Linux shares with illumos keep illumos'
 * values (<sys/errno.h>), so a driver's negative errno reaches user space
 * unchanged. The Linux-only names get the kernel-internal values Linux
 * gives them (512 and up) or, where Linux's value is a user-visible errno
 * illumos lacks, a value from 600 up; lkpi_errno() maps all of them onto an
 * illumos errno before a value leaves the layer (ioctl results, see
 * <lkpi/drm.h>).
 */
#ifndef _LINUX_ERRNO_H
#define	_LINUX_ERRNO_H

#include <sys/errno.h>

#define	ERESTARTSYS		512
#define	ERESTARTNOINTR		513
#define	ERESTARTNOHAND		514
#define	ENOIOCTLCMD		515
#define	ERESTART_RESTARTBLOCK	516
#define	EPROBE_DEFER		517
#define	EOPENSTALE		518
#define	ENOPARAM		519
#define	EBADHANDLE		521
#define	ENOTSYNC		522
#define	EBADCOOKIE		523
#define	ENOTSUPP		524
#define	ETOOSMALL		525
#define	ESERVERFAULT		526
#define	EBADTYPE		527
#define	EJUKEBOX		528
#define	EIOCBQUEUED		529
#define	ERECALLCONFLICT		530

#ifndef	EREMOTEIO
#define	EREMOTEIO		600
#endif
#ifndef	EUCLEAN
#define	EUCLEAN			601
#endif
#ifndef	ENOMEDIUM
#define	ENOMEDIUM		602
#endif
#ifndef	EMEDIUMTYPE
#define	EMEDIUMTYPE		603
#endif
#ifndef	ENOKEY
#define	ENOKEY			604
#endif
#ifndef	EKEYEXPIRED
#define	EKEYEXPIRED		605
#endif
#ifndef	EHWPOISON
#define	EHWPOISON		606
#endif
#ifndef	ERFKILL
#define	ERFKILL			607
#endif
#ifndef	EBADE
#define	EBADE			608
#endif
#ifndef	EBADR
#define	EBADR			609
#endif
#ifndef	ENAVAIL
#define	ENAVAIL			610
#endif
#ifndef	ENOTNAM
#define	ENOTNAM			611
#endif
#ifndef	EISNAM
#define	EISNAM			612
#endif
#ifndef	ENOTRECOVERABLE
#define	ENOTRECOVERABLE		613
#endif
#ifndef	EOWNERDEAD
#define	EOWNERDEAD		614
#endif

/*
 * The illumos errno for a Linux one: the identity for illumos' own
 * numbers, the nearest illumos meaning for the Linux-only ones.
 */
static inline int
lkpi_errno(int e)
{
	switch (e) {
	case ERESTARTSYS:
	case ERESTARTNOINTR:
	case ERESTARTNOHAND:
	case ERESTART_RESTARTBLOCK:
		return (EINTR);
	case ENOIOCTLCMD:
		return (ENOTTY);
	case EPROBE_DEFER:
		return (EAGAIN);
	case ENOTSUPP:
	case EBADTYPE:
		return (ENOTSUP);
	case ETOOSMALL:
		return (EINVAL);
	case EREMOTEIO:
	case EUCLEAN:
	case EHWPOISON:
	case EBADE:
	case EBADR:
	case ESERVERFAULT:
		return (EIO);
	case ENOMEDIUM:
	case EMEDIUMTYPE:
		return (ENODEV);
	default:
		return (e >= 512 ? EIO : e);
	}
}

#endif
