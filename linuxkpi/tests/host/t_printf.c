/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_printf.c: the C99 conversions, flags, width and
 * precision (compared with the host C library where the result is
 * defined by C), return values and truncation, Linux's %p extensions,
 * kasprintf(), printk() levels and the hex dumps.
 */

#include <linux/kernel.h>
#include <linux/sprintf.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <drm/drm_print.h>
#include "test.h"

/* The host C library's snprintf, for reference results. */
#undef snprintf
#define	host_snprintf	__builtin_snprintf

static char buf[512];

#define	FMT(expect, ...)	do { \
	int __n = lkpi_snprintf(buf, sizeof (buf), __VA_ARGS__); \
	CHECK_STR(buf, expect); \
	CHECK_EQ(__n, (int)strlen(expect)); \
	} while (0)

/* Same result as the host C library. */
#define	SAME(...)	do { \
	char __ref[512]; \
	int __r = host_snprintf(__ref, sizeof (__ref), __VA_ARGS__); \
	int __n = lkpi_snprintf(buf, sizeof (buf), __VA_ARGS__); \
	CHECK_STR(buf, __ref); \
	CHECK_EQ(__n, __r); \
	} while (0)

TEST(printf_c99)
{
	SAME("%d %i %u", 0, -42, 42u);
	SAME("%d %d", INT_MAX, INT_MIN);
	SAME("%ld %lu %lld %llu", LONG_MIN, ULONG_MAX, LLONG_MIN, ULLONG_MAX);
	SAME("%hhd %hhu %hd %hu", 300, 300, 70000, 70000);
	SAME("%zu %zd %td %jd", (size_t)123, (ssize_t)-5, (ptrdiff_t)-7,
	    (intmax_t)99);
	SAME("%x %X %o %#x %#X %#o", 255u, 255u, 8u, 255u, 255u, 8u);
	SAME("%#x %#o", 0u, 0u);
	SAME("[%5d] [%-5d] [%05d] [%+d] [% d] [%+5d] [%-+5d]", 42, 42, 42, 42,
	    42, 42, 42);
	SAME("[%05d] [%5.3d] [%-8.3x] [%08.3d] [%.0d] [%.0x]", -42, 7, 10u, 5,
	    0, 0u);
	SAME("[%#.3o] [%#5x] [%#08x] [%-#8o]", 8u, 1u, 1u, 9u);
	SAME("[%*d] [%-*d] [%*d] [%.*d] [%.*d]", 6, 1, 6, 2, -6, 3, 4, 5, -1,
	    6);
	SAME("[%c] [%3c] [%-3c] [%%]", 'a', 'b', 'c');
	SAME("[%s] [%10s] [%-10s] [%.2s] [%10.3s] [%*.*s]", "abc", "abc", "abc",
	    "abc", "abcdef", 6, 1, "xyz");
	SAME("%s%s", "", "");
	SAME("%llx %llX", 0xdeadbeefcafebabeULL, 0xdeadbeefcafebabeULL);
	SAME("%lo", 01777777777777777777777UL);
	SAME("%+.0d|% .0d", 0, 0);
	FMT("%|%|", "%5%|%-5%|");	/* %% ignores flags, as Linux */
	FMT("(null)", "%s", (char *)NULL);
	FMT("  (nul", "%6.4s", (char *)NULL);
	FMT("%y", "%y");
	FMT("abc%", "abc%");
}

TEST(printf_return)
{
	char small[8];
	int n;

	n = lkpi_snprintf(small, sizeof (small), "%s", "0123456789");
	CHECK_EQ(n, 10);
	CHECK_STR(small, "0123456");
	n = lkpi_snprintf(NULL, 0, "%d-%s", 12345, "abc");
	CHECK_EQ(n, 9);
	small[0] = 'x';
	n = lkpi_snprintf(small, 1, "abc");
	CHECK_EQ(n, 3);
	CHECK_EQ(small[0], '\0');
	n = lkpi_scnprintf(small, sizeof (small), "%s", "0123456789");
	CHECK_EQ(n, 7);
	n = lkpi_scnprintf(small, sizeof (small), "%s", "012");
	CHECK_EQ(n, 3);
	n = lkpi_scnprintf(small, 0, "%s", "012");
	CHECK_EQ(n, 0);
	n = lkpi_sprintf(buf, "%d/%d", 1, 2);
	CHECK_EQ(n, 3);
	CHECK_STR(buf, "1/2");
	lkpi_host_cmn_quiet = 1;
	n = lkpi_snprintf(buf, (size_t)INT_MAX + 1, "x");
	lkpi_host_cmn_quiet = 0;
	CHECK_EQ(n, 0);
}

static int
vafmt(char *out, size_t size, const char *fmt, ...)
{
	struct va_format vaf;
	va_list ap;
	int n;

	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	n = lkpi_snprintf(out, size, "<%pV>", &vaf);
	va_end(ap);
	return (n);
}

TEST(printf_pext)
{
	phys_addr_t pa = 0x12345678abULL;
	dma_addr_t da = 0xfeedULL;
	u8 bytes[70];
	u32 fourcc = 0x34325258;	/* "XR24" */
	u32 fourcc_be = 0x34325258 | (1U << 31);
	struct resource r = { .start = 0xf0000000, .end = 0xf0ffffff,
	    .flags = IORESOURCE_MEM };
	struct resource io = { .start = 0x3c0, .end = 0x3df,
	    .flags = IORESOURCE_IO };
	u8 uuid[16];
	void *p = (void *)0xffffff0012345678UL;
	int i, n;

	for (i = 0; i < 70; i++)
		bytes[i] = (u8)i;
	for (i = 0; i < 16; i++)
		uuid[i] = (u8)(0x10 + i);

	FMT("0x00000012345678ab", "%pa", &pa);
	FMT("0x00000012345678ab", "%pap", &pa);
	FMT("0x000000000000feed", "%pad", &da);
	FMT("-ENOMEM", "%pe", ERR_PTR(-ENOMEM));
	FMT("-EINVAL", "%pe", ERR_PTR(-EINVAL));
	FMT("-ETIMEDOUT", "%pe", ERR_PTR(-ETIMEDOUT));
	FMT("-EPROBE_DEFER", "%pe", ERR_PTR(-EPROBE_DEFER));
	FMT("-4000", "%pe", ERR_PTR(-4000));
	FMT("[-ENOSPC]", "[%pe]", ERR_PTR(-ENOSPC));
	FMT("00", "%ph", bytes);
	FMT("00 01 02 03", "%4ph", bytes);
	FMT("00:01:02", "%3phC", bytes);
	FMT("00-01-02", "%3phD", bytes);
	FMT("000102", "%3phN", bytes);
	FMT("0a0b", "%*phN", 2, bytes + 10);
	n = lkpi_snprintf(buf, sizeof (buf), "%70phN", bytes);
	CHECK_EQ(n, 128);		/* at most 64 bytes */
	FMT("", "%*ph", 0, bytes);
	FMT("00", "%0ph", bytes);	/* 0 is a flag, not a width */
	n = vafmt(buf, sizeof (buf), "a=%d b=%s", 1, "xy");
	CHECK_STR(buf, "<a=1 b=xy>");
	CHECK_EQ(n, 10);
	FMT("ffffff0012345678", "%p", p);
	FMT("ffffff0012345678", "%px", p);
	FMT("ffffff0012345678", "%pK", p);
	FMT("0x0000000000001234", "%pS", (void *)0x1234);
	FMT("0x0000000000001234", "%ps", (void *)0x1234);
	FMT("0x0000000000001234", "%pSR", (void *)0x1234);
	FMT("0x0000000000001234", "%pB", (void *)0x1234);
	FMT("[mem 0xf0000000-0xf0ffffff flags 0x200]", "%pR", &r);
	FMT("[io 0x03c0-0x03df flags 0x100]", "%pR", &io);
	FMT("10111213-1415-1617-1819-1a1b1c1d1e1f", "%pU", uuid);
	FMT("10111213-1415-1617-1819-1a1b1c1d1e1f", "%pUb", uuid);
	FMT("13121110-1514-1716-1819-1a1b1c1d1e1f", "%pUl", uuid);
	FMT("13121110-1514-1716-1819-1A1B1C1D1E1F", "%pUL", uuid);
	FMT("XR24 (little-endian)", "%p4cc", &fourcc);
	FMT("XR24 (big-endian)", "%p4cc", &fourcc_be);
	/* Unknown extensions print the pointer and skip their letters. */
	FMT("ffffff0012345678.", "%pZz9.", p);
	FMT("          (null) end", "%p end", NULL);
	FMT("0000000000000000", "%px", NULL);
	/* NULL and bad pointers do not fault. */
	FMT("(null)", "%pa", NULL);
	FMT("(null)", "%ph", NULL);
	FMT("(efault)", "%pR", (void *)16);
	FMT("(efault)", "%pU", ERR_PTR(-EIO));
	FMT("(efault)", "%pV", (void *)8);
	FMT("x(null)y", "x%sy", (char *)NULL);
	FMT("(efault)", "%s", (char *)ERR_PTR(-EINVAL));
	/* A width on a plain %p pads as a number. */
	FMT("     0x10", "%9s", "0x10");
	FMT("      10", "%8p", (void *)0x10);
}

static int
fmt_alloc_check(void)
{
	char *s = kasprintf(GFP_KERNEL, "%s-%d", "card", 3);
	int ok = s != NULL && strcmp(s, "card-3") == 0;

	kfree(s);
	return (ok);
}

TEST(printk)
{
	unsigned long c0;
	char big[700];
	u8 data[40];
	char line[200];
	int i, n;

	CHECK(fmt_alloc_check());
	lkpi_host_cmn_quiet = 1;
	c0 = lkpi_host_cmn_count;
	printk(KERN_ERR "error %d\n", 5);
	CHECK_EQ(lkpi_host_cmn_level, CE_WARN);
	CHECK_STR(lkpi_host_cmn_msg, "error 5");
	pr_warn("warn\n");
	CHECK_EQ(lkpi_host_cmn_level, CE_WARN);
	pr_notice("note\n");
	CHECK_EQ(lkpi_host_cmn_level, CE_NOTE);
	CHECK_STR(lkpi_host_cmn_msg, "note");
	pr_info("info %s\n", "x");
	CHECK_EQ(lkpi_host_cmn_level, CE_NOTE);
	CHECK_STR(lkpi_host_cmn_msg, "!info x");
	printk("%s" "level via arg\n", KERN_DEBUG);
	CHECK_STR(lkpi_host_cmn_msg, "!level via arg");
	pr_cont("cont");
	CHECK_EQ(lkpi_host_cmn_level, CE_CONT);
	CHECK_STR(lkpi_host_cmn_msg, "cont");
	printk("plain\n");
	CHECK_EQ(lkpi_host_cmn_level, CE_NOTE);
	CHECK_STR(lkpi_host_cmn_msg, "plain");
	pr_debug("hidden\n");
	CHECK_EQ(lkpi_host_cmn_count, c0 + 7);
	lkpi_debug = 1;
	pr_debug("shown\n");
	lkpi_debug = 0;
	CHECK_STR(lkpi_host_cmn_msg, "!shown");
	/* Long messages are cut and end in "...". */
	memset(big, 'a', sizeof (big) - 1);
	big[sizeof (big) - 1] = '\0';
	n = printk(KERN_ERR "%s\n", big);
	CHECK_EQ(strlen(lkpi_host_cmn_msg), 508);
	CHECK(strcmp(lkpi_host_cmn_msg + 505, "...") == 0);
	CHECK_EQ(n, 508);

	/* WARN() reports file and line, BUG() is not tested (panics). */
	CHECK(WARN(1, "bad %d", 3));
	CHECK(strstr(lkpi_host_cmn_msg, "t_printf.c") != NULL);
	CHECK(strstr(lkpi_host_cmn_msg, ": bad 3") != NULL);
	CHECK(!WARN_ON(0));
	CHECK(WARN_ON(2 > 1));
	CHECK(strstr(lkpi_host_cmn_msg, "WARN_ON(2 > 1)") != NULL);

	/* Hex dumps in Linux's layout. */
	for (i = 0; i < 40; i++)
		data[i] = (u8)(0x41 + i);
	n = hex_dump_to_buffer(data, 16, 16, 1, line, sizeof (line), true);
	CHECK_STR(line, "41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f 50  "
	    "ABCDEFGHIJKLMNOP");
	CHECK_EQ(n, (int)strlen(line));
	n = hex_dump_to_buffer(data, 8, 16, 4, line, sizeof (line), false);
	CHECK_STR(line, "44434241 48474645");
	n = hex_dump_to_buffer(data, 8, 16, 8, line, sizeof (line), true);
	CHECK_STR(line, "4847464544434241" "                   " "ABCDEFGH");
	n = hex_dump_to_buffer(data, 3, 16, 2, line, sizeof (line), false);
	CHECK_STR(line, "41 42 43");
	n = hex_dump_to_buffer(data, 16, 16, 1, line, 10, false);
	CHECK_EQ(n, 47);
	CHECK_STR(line, "41 42 43 ");
	c0 = lkpi_host_cmn_count;
	print_hex_dump(KERN_INFO, "pfx: ", DUMP_PREFIX_OFFSET, 16, 1, data,
	    40, true);
	CHECK_EQ(lkpi_host_cmn_count, c0 + 3);
	CHECK_STR(lkpi_host_cmn_msg, "!pfx: 00000020: 61 62 63 64 65 66 67 68"
	    "                          " "abcdefgh");
	lkpi_host_cmn_quiet = 0;
}
