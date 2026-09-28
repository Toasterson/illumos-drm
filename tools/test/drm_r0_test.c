/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Userland test of the illumos-drm R0 core, run as root against rdrmnull.
 *
 * It uses the installed libdrm 2.4.109 (drmOpen, drmGetVersion, drmGetCap,
 * drmSetClientCap, drmGetMagic, drmAuthMagic, drmGetClient, drmSetMaster,
 * drmDropMaster, drmSetInterfaceVersion, drmGetBusid, drmSetBusid) and raw
 * ioctls for the error paths. Unprivileged steps run in a child that has
 * switched to nobody (60001). Every check prints PASS or FAIL; the exit
 * status is the number of failures.
 *
 * Build on illumos, with the patched libdrm headers from
 * tools/uapi/generate.sh in $INC:
 *   gcc -m64 -Wall -I$INC -o drm_r0_test drm_r0_test.c \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread
 * and the 32-bit probe (no libdrm):
 *   gcc -m32 -Wall -DPROBE32 -I$INC -o drm_r0_probe32 drm_r0_test.c
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mkdev.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioccom.h>

#include "drm.h"

#define	CARD	"/dev/dri/card0"
#define	RENDER	"/dev/dri/renderD128"
#define	BUSID	"pseudo:rdrmnull@0"
#define	NOBODY	60001

static int failures;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
	(void) fflush(stdout);
}

#ifdef PROBE32

/*
 * 32-bit processes are refused with ENOTSUP for every core command,
 * because only the LP64 ABI exists; other commands are ENOTTY.
 */
int
main(void)
{
	struct drm_version v;
	struct drm_get_cap cap;
	int fd = open(CARD, O_RDWR);

	check(fd >= 0, "32-bit open " CARD);
	if (fd < 0)
		return (1);
	(void) memset(&v, 0, sizeof (v));
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_VERSION, &v) == -1 && errno == ENOTSUP,
	    "32-bit DRM_IOCTL_VERSION fails with ENOTSUP");
	(void) memset(&cap, 0, sizeof (cap));
	cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == -1 && errno == ENOTSUP,
	    "32-bit DRM_IOCTL_GET_CAP fails with ENOTSUP");
	errno = 0;
	check(ioctl(fd, _IO('d', 0xfe)) == -1 && errno == ENOTTY,
	    "32-bit unknown DRM ioctl fails with ENOTTY");
	check(!isatty(fd), "32-bit isatty() is false (TCGETA is ENOTTY)");
	(void) close(fd);
	(void) printf("probe32: %d failures\n", failures);
	return (failures);
}

#else	/* PROBE32 */

#include <xf86drm.h>

/* libdrm returns either -errno or -1 with errno set, depending on the call. */
static int
is_err(int ret, int e)
{
	return (ret == -e || (ret == -1 && errno == e));
}

static void
drop_to_nobody(void)
{
	if (setgid(NOBODY) != 0 || setuid(NOBODY) != 0) {
		perror("setuid nobody");
		exit(100);
	}
}

static void
print_minor(const char *what, int fd)
{
	struct stat st;

	if (fstat(fd, &st) == 0)
		(void) printf("INFO %s: rdev major %u minor 0x%x (node %u, clone %u)\n",
		    what, (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev),
		    (unsigned)(minor(st.st_rdev) & 0x1ff),
		    (unsigned)((minor(st.st_rdev) >> 9) & 0x1ff));
}

static void
test_version(int fd, const char *node)
{
	char what[128];
	drmVersionPtr v = drmGetVersion(fd);

	(void) snprintf(what, sizeof (what), "drmGetVersion on %s", node);
	check(v != NULL, what);
	if (v == NULL)
		return;
	(void) printf("INFO version %d.%d.%d name=\"%s\" date=\"%s\" desc=\"%s\"\n",
	    v->version_major, v->version_minor, v->version_patchlevel,
	    v->name, v->date, v->desc);
	check(strcmp(v->name, "rdrmnull") == 0 && v->version_major == 1 &&
	    v->version_minor == 0 && strcmp(v->date, "20260928") == 0 &&
	    v->name_len == 8, "version fields are those of rdrmnull");
	drmFreeVersion(v);
}

static void
test_caps(int fd, const char *node)
{
	static const struct { uint64_t cap; uint64_t want; const char *name; } caps[] = {
		{ DRM_CAP_TIMESTAMP_MONOTONIC, 1, "TIMESTAMP_MONOTONIC" },
		{ DRM_CAP_PRIME, 0, "PRIME" },
		{ DRM_CAP_SYNCOBJ, 0, "SYNCOBJ" },
		{ DRM_CAP_SYNCOBJ_TIMELINE, 0, "SYNCOBJ_TIMELINE" },
		{ DRM_CAP_CURSOR_WIDTH, 64, "CURSOR_WIDTH" },
		{ DRM_CAP_CURSOR_HEIGHT, 64, "CURSOR_HEIGHT" },
		{ DRM_CAP_ADDFB2_MODIFIERS, 0, "ADDFB2_MODIFIERS" },
		{ DRM_CAP_DUMB_BUFFER, 0, "DUMB_BUFFER" },
		{ DRM_CAP_CRTC_IN_VBLANK_EVENT, 0, "CRTC_IN_VBLANK_EVENT" },
	};
	char what[128];
	uint64_t val;
	int i, r;

	for (i = 0; i < (int)(sizeof (caps) / sizeof (caps[0])); i++) {
		val = 0xdeadbeef;
		r = drmGetCap(fd, caps[i].cap, &val);
		(void) snprintf(what, sizeof (what), "drmGetCap(%s) on %s = %llu",
		    caps[i].name, node, (unsigned long long)val);
		check(r == 0 && val == caps[i].want, what);
	}
	errno = 0;
	r = drmGetCap(fd, 0x15, &val);
	(void) snprintf(what, sizeof (what), "drmGetCap(0x15, unknown) on %s is EINVAL", node);
	check(is_err(r, EINVAL), what);
}

static void
test_client_caps(int fd)
{
	errno = 0;
	check(is_err(drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1), EINVAL),
	    "SET_CLIENT_CAP WRITEBACK_CONNECTORS before ATOMIC is EINVAL");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0,
	    "SET_CLIENT_CAP UNIVERSAL_PLANES=1");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "SET_CLIENT_CAP ATOMIC=1");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ASPECT_RATIO, 1) == 0,
	    "SET_CLIENT_CAP ASPECT_RATIO=1");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1) == 0,
	    "SET_CLIENT_CAP WRITEBACK_CONNECTORS=1 after ATOMIC");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_STEREO_3D, 1) == 0, "SET_CLIENT_CAP STEREO_3D=1");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 2) == 0, "SET_CLIENT_CAP ATOMIC=2 (Linux's Xorg value)");
	errno = 0;
	check(is_err(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 3), EINVAL),
	    "SET_CLIENT_CAP ATOMIC=3 is EINVAL");
	errno = 0;
	check(is_err(drmSetClientCap(fd, 99, 1), EINVAL), "SET_CLIENT_CAP 99 (unknown) is EINVAL");
}

static int
get_auth(int fd)
{
	int auth = -1, pid, uid;
	unsigned long magic, iocs;

	if (drmGetClient(fd, 0, &auth, &pid, &uid, &magic, &iocs) != 0)
		return (-1);
	return (auth);
}

/* Unprivileged client: magic handshake with the root master over pipes. */
static void
auth_client(int to_parent, int from_parent)
{
	drm_magic_t magic = 0;
	char ok;
	int fd, auth;

	drop_to_nobody();
	fd = open(CARD, O_RDWR);
	check(fd >= 0, "nobody: open " CARD);
	if (fd < 0)
		exit(failures);
	print_minor("nobody card0 client", fd);
	test_version(fd, "card0 as nobody");
	check(get_auth(fd) == 0, "nobody: client is not authenticated after open");
	check(drmGetMagic(fd, &magic) == 0 && magic != 0, "nobody: drmGetMagic");
	(void) printf("INFO nobody magic %u\n", magic);
	errno = 0;
	check(is_err(drmAuthMagic(fd, magic), EACCES), "nobody: drmAuthMagic by a non-master is EACCES");
	errno = 0;
	check(is_err(drmSetMaster(fd), EACCES), "nobody: drmSetMaster is EACCES");
	errno = 0;
	check(is_err(drmDropMaster(fd), EACCES), "nobody: drmDropMaster is EACCES");
	(void) write(to_parent, &magic, sizeof (magic));
	(void) read(from_parent, &ok, 1);
	auth = get_auth(fd);
	check(auth == 1, "nobody: client is authenticated after the master's drmAuthMagic");
	(void) close(fd);

	/* Render node as nobody: no authentication, no master, no magic. */
	fd = drmOpenRender(128);
	check(fd >= 0, "nobody: drmOpenRender(128)");
	if (fd >= 0) {
		print_minor("nobody renderD128", fd);
		test_version(fd, "renderD128 as nobody");
		test_caps(fd, "renderD128 as nobody");
		errno = 0;
		check(is_err(drmGetMagic(fd, &magic), EACCES), "nobody: GET_MAGIC on render is EACCES");
		errno = 0;
		check(is_err(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1), EACCES),
		    "nobody: SET_CLIENT_CAP on render is EACCES");
		check(drmGetBusid(fd) == NULL, "nobody: GET_UNIQUE on render fails");
		(void) close(fd);
	}
	exit(failures);
}

struct stress {
	int id;
	int errors;
	int opens;
	int auths;
};

static int master_fd;

/*
 * One of eight threads of an unprivileged process. Each iteration opens a
 * card or render file, queries it, and for card files asks the root
 * master (whose file this process inherited) to authenticate its magic
 * number. The parent process drops and retakes master concurrently, so a
 * card open can find the device without master and become master itself;
 * AUTH_MAGIC may therefore fail with EACCES (master_fd is not master at
 * the moment) or EINVAL (the magic belongs to another master's namespace),
 * but whenever it succeeds the file must be authenticated.
 */
static void *
stress_thread(void *arg)
{
	struct stress *s = arg;
	int i, fd, r;
	uint64_t val;
	drm_magic_t magic;

	for (i = 0; i < 400; i++) {
		int render = (i + s->id) % 3 == 0;
		fd = open(render ? RENDER : CARD, O_RDWR);
		if (fd < 0) {
			(void) printf("INFO thread %d: open errno %d\n", s->id, errno);
			s->errors++;
			continue;
		}
		s->opens++;
		if (drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &val) != 0 || val != 1)
			s->errors++;
		if (!render) {
			if (drmGetMagic(fd, &magic) != 0 || magic == 0) {
				s->errors++;
			} else {
				r = drmAuthMagic(master_fd, magic);
				if (r == 0) {
					s->auths++;
					if (get_auth(fd) != 1) {
						(void) printf("INFO thread %d: AUTH_MAGIC succeeded but the file is not authenticated\n", s->id);
						s->errors++;
					}
				} else if (r != -EACCES && r != -EINVAL) {
					(void) printf("INFO thread %d: AUTH_MAGIC %d\n", s->id, r);
					s->errors++;
				}
			}
		}
		(void) close(fd);
	}
	return (NULL);
}

/* The unprivileged stress process; exits with the number of failures. */
static void
stress_child(void)
{
	pthread_t th[8];
	struct stress st[8];
	int i, n = 0, auths = 0;

	drop_to_nobody();
	for (i = 0; i < 8; i++) {
		(void) memset(&st[i], 0, sizeof (st[i]));
		st[i].id = i;
		(void) pthread_create(&th[i], NULL, stress_thread, &st[i]);
	}
	for (i = 0; i < 8; i++) {
		(void) pthread_join(th[i], NULL);
		n += st[i].opens;
		auths += st[i].auths;
		if (st[i].errors != 0)
			(void) printf("INFO stress thread %d: %d errors\n", i, st[i].errors);
		check(st[i].errors == 0, "nobody: stress thread without unexpected errors");
	}
	(void) printf("INFO stress: %d opens and closes, %d authentications in 8 threads as nobody\n",
	    n, auths);
	check(auths > 0, "nobody: some authentications succeeded during the stress run");
	exit(failures);
}

/*
 * `drm_r0_test hold <path>`: open <path>, print the minor number, and keep
 * the file open until stdin reaches end of file. run-r0-test.sh uses it to
 * check that the driver cannot be unloaded while a file is open.
 */
static int
hold(const char *path)
{
	char c;
	int fd = open(path, O_RDWR);

	if (fd < 0) {
		perror(path);
		return (1);
	}
	print_minor("holding", fd);
	while (read(0, &c, 1) > 0)
		;
	(void) close(fd);
	return (0);
}

int
main(int argc, char **argv)
{
	struct rlimit rl = { 4096, 4096 };
	drmSetVersion sv;
	char *busid;
	int fd, fd2, rfd, i, n, status, p2c[2], c2p[2];
	drm_magic_t magic;
	pid_t pid;
	static int fds[600];

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 3 && strcmp(argv[1], "hold") == 0)
		return (hold(argv[2]));

	/* drmOpen by name: first primary open, so this file is master. */
	fd = drmOpen("rdrmnull", NULL);
	check(fd >= 0, "drmOpen(\"rdrmnull\", NULL) as root");
	if (fd < 0)
		return (1);
	master_fd = fd;
	print_minor("root card0 master", fd);
	{
		char *name = drmGetDeviceNameFromFd(fd);
		(void) printf("INFO drmGetDeviceNameFromFd: %s\n", name ? name : "(null)");
		check(name != NULL && strcmp(name, CARD) == 0,
		    "drmGetDeviceNameFromFd strips the clone bits and finds " CARD);
		free(name);
	}
	test_version(fd, "card0 as root");
	test_caps(fd, "card0 as root");
	test_client_caps(fd);
	check(get_auth(fd) == 1, "root master is authenticated");

	busid = drmGetBusid(fd);
	check(busid != NULL && busid[0] == '\0', "GET_UNIQUE is empty before SET_VERSION 1.1");
	drmFreeBusid(busid);

	(void) memset(&sv, 0, sizeof (sv));
	sv.drm_di_major = 1;
	sv.drm_di_minor = 4;
	sv.drm_dd_major = -1;
	sv.drm_dd_minor = -1;
	check(drmSetInterfaceVersion(fd, &sv) == 0 && sv.drm_di_major == 1 && sv.drm_di_minor == 4 &&
	    sv.drm_dd_major == 1 && sv.drm_dd_minor == 0, "drmSetInterfaceVersion 1.4 by the master");
	sv.drm_di_major = 2;
	sv.drm_di_minor = 0;
	errno = 0;
	check(is_err(drmSetInterfaceVersion(fd, &sv), EINVAL) && sv.drm_di_major == 1,
	    "drmSetInterfaceVersion 2.0 is EINVAL and reports 1.4");
	busid = drmGetBusid(fd);
	(void) printf("INFO busid \"%s\"\n", busid ? busid : "(null)");
	check(busid != NULL && strcmp(busid, BUSID) == 0, "GET_UNIQUE reports " BUSID " after SET_VERSION 1.4");
	drmFreeBusid(busid);
	errno = 0;
	check(is_err(drmSetBusid(fd, "pci:0000:00:02.0"), EINVAL), "SET_UNIQUE with a foreign bus ID is EINVAL");
	errno = 0;
	check(is_err(drmSetBusid(fd, BUSID), EBUSY), "SET_UNIQUE with the bus ID once set is EBUSY");

	/* Raw error paths. */
	{
		char buf[64];
		errno = 0;
		check(ioctl(fd, _IOWR('d', 0x99, struct drm_version), buf) == -1 && errno == ENOTTY,
		    "unknown DRM ioctl 0x99 is ENOTTY");
		errno = 0;
		check(ioctl(fd, _IOWR('d', 0x00, char[36]), buf) == -1 && errno == ENOTTY,
		    "DRM_IOCTL_VERSION with a 36-byte (ILP32) size is ENOTTY");
		errno = 0;
		check(ioctl(fd, DRM_IOCTL_GEM_CLOSE, buf) == -1 && errno == ENOTTY,
		    "DRM_IOCTL_GEM_CLOSE (R1) is ENOTTY");
		errno = 0;
		check(ioctl(fd, DRM_IOCTL_VERSION, (void *)8) == -1 && errno == EFAULT,
		    "DRM_IOCTL_VERSION with a bad pointer is EFAULT");
	}

	/* Magic handshake with an unprivileged client. */
	if (pipe(p2c) != 0 || pipe(c2p) != 0)
		return (1);
	pid = fork();
	if (pid == 0) {
		(void) close(p2c[1]);
		(void) close(c2p[0]);
		auth_client(c2p[1], p2c[0]);
	}
	(void) close(p2c[0]);
	(void) close(c2p[1]);
	magic = 0;
	(void) read(c2p[0], &magic, sizeof (magic));
	check(drmAuthMagic(fd, magic) == 0, "root master: drmAuthMagic(client magic)");
	errno = 0;
	check(is_err(drmAuthMagic(fd, magic), EINVAL), "root master: the same magic again is EINVAL");
	(void) write(p2c[1], "x", 1);
	(void) waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unprivileged client checks (see above)");

	/* Master hand-over between two root files. */
	fd2 = open(CARD, O_RDWR);
	check(fd2 >= 0, "root: second open of " CARD);
	errno = 0;
	check(is_err(drmSetMaster(fd2), EBUSY), "second file: drmSetMaster while another is master is EBUSY");
	check(drmDropMaster(fd) == 0, "master: drmDropMaster");
	errno = 0;
	check(is_err(drmDropMaster(fd), EINVAL), "former master: drmDropMaster again is EINVAL");
	check(drmSetMaster(fd2) == 0, "second file (privileged): drmSetMaster");
	errno = 0;
	check(is_err(drmSetMaster(fd), EBUSY), "former master: drmSetMaster while fd2 is master is EBUSY");
	busid = drmGetBusid(fd2);
	check(busid != NULL && busid[0] == '\0', "a new master object starts without a bus ID");
	drmFreeBusid(busid);
	check(drmDropMaster(fd2) == 0 && drmSetMaster(fd) == 0, "hand master back to the first file");
	(void) close(fd2);

	/* Render node as root. */
	rfd = drmOpenRender(128);
	check(rfd >= 0, "root: drmOpenRender(128)");
	if (rfd >= 0) {
		print_minor("root renderD128", rfd);
		test_version(rfd, "renderD128 as root");
		test_caps(rfd, "renderD128 as root");
		errno = 0;
		check(is_err(drmGetMagic(rfd, &magic), EACCES), "root: GET_MAGIC on render is EACCES");
		errno = 0;
		check(is_err(drmSetMaster(rfd), EACCES), "root: SET_MASTER on render is EACCES");
		(void) close(rfd);
	}

	/*
	 * Concurrent opens, ioctls and closes from eight unprivileged threads,
	 * while this (root, opening) process drops and retakes master.
	 */
	pid = fork();
	if (pid == 0)
		stress_child();
	n = 0;
	i = 0;
	while (waitpid(pid, &status, WNOHANG) == 0) {
		(void) drmDropMaster(fd);
		if (drmSetMaster(fd) == 0)
			n++;
		else
			i++;
	}
	(void) printf("INFO master toggles during the stress run: %d retaken, %d refused (EBUSY)\n", n, i);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unprivileged stress run (see above)");
	check(drmSetMaster(fd) == 0 && get_auth(fd) == 1, "master is retaken once the stress files are closed");

	/* The clone id space: 511 files per device, then EMFILE. */
	(void) setrlimit(RLIMIT_NOFILE, &rl);
	n = 0;
	for (i = 0; i < 600; i++) {
		fds[i] = open((i & 1) ? RENDER : CARD, O_RDWR);
		if (fds[i] < 0)
			break;
		n++;
	}
	(void) printf("INFO %d further opens succeeded (1 file already open), then errno %d\n", n, errno);
	check(n == 510 && errno == EMFILE, "a device holds 511 open files, the next open is EMFILE");
	while (--i >= 0)
		(void) close(fds[i]);
	fd2 = open(CARD, O_RDWR);
	check(fd2 >= 0, "opens work again after closing");
	(void) close(fd2);

	(void) close(fd);
	/* With every file closed, the next primary open is master again. */
	fd = open(CARD, O_RDWR);
	check(fd >= 0 && drmSetMaster(fd) == 0 && get_auth(fd) == 1, "fresh open after all closes is master");
	(void) close(fd);

	(void) printf("drm_r0_test: %d failures\n", failures);
	return (failures);
}

#endif	/* PROBE32 */
