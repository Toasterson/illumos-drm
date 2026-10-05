/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_string.c: kstrto*() (bases, signs, ranges, the
 * trailing newline), simple_strto*(), the string helpers, the hex
 * helpers, string_get_size() and sscanf().
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/string_helpers.h>
#include <linux/kstrtox.h>
#include <linux/hex.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include "test.h"

TEST(kstrtox)
{
	unsigned long long ull;
	long long ll;
	unsigned int ui;
	int i;
	u16 u16v;
	u8 u8v;
	s8 s8v;
	bool b;
	unsigned long ul;
	long l;

	CHECK_EQ(kstrtoull("12345", 10, &ull), 0);
	CHECK_EQ(ull, 12345);
	CHECK_EQ(kstrtoull("12345\n", 10, &ull), 0);
	CHECK_EQ(kstrtoull("12345\n\n", 10, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("+7", 10, &ull), 0);
	CHECK_EQ(ull, 7);
	CHECK_EQ(kstrtoull("-7", 10, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("", 10, &ull), -EINVAL);
	CHECK_EQ(kstrtoull(" 1", 10, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("1 ", 10, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("0x1f", 0, &ull), 0);
	CHECK_EQ(ull, 31);
	CHECK_EQ(kstrtoull("0X1F", 16, &ull), 0);
	CHECK_EQ(ull, 31);
	CHECK_EQ(kstrtoull("1f", 16, &ull), 0);
	CHECK_EQ(ull, 31);
	CHECK_EQ(kstrtoull("017", 0, &ull), 0);
	CHECK_EQ(ull, 15);
	CHECK_EQ(kstrtoull("0", 0, &ull), 0);
	CHECK_EQ(ull, 0);
	CHECK_EQ(kstrtoull("08", 0, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("0x", 0, &ull), -EINVAL);
	CHECK_EQ(kstrtoull("18446744073709551615", 10, &ull), 0);
	CHECK(ull == ULLONG_MAX);
	CHECK_EQ(kstrtoull("18446744073709551616", 10, &ull), -ERANGE);
	CHECK_EQ(kstrtoull("ffffffffffffffff", 16, &ull), 0);
	CHECK_EQ(kstrtoull("10000000000000000", 16, &ull), -ERANGE);
	CHECK_EQ(kstrtoull("777", 8, &ull), 0);
	CHECK_EQ(ull, 511);
	CHECK_EQ(kstrtoull("8", 8, &ull), -EINVAL);

	CHECK_EQ(kstrtoll("-9223372036854775808", 10, &ll), 0);
	CHECK(ll == LLONG_MIN);
	CHECK_EQ(kstrtoll("-9223372036854775809", 10, &ll), -ERANGE);
	CHECK_EQ(kstrtoll("9223372036854775807", 10, &ll), 0);
	CHECK(ll == LLONG_MAX);
	CHECK_EQ(kstrtoll("9223372036854775808", 10, &ll), -ERANGE);
	CHECK_EQ(kstrtoll("-0x10", 0, &ll), 0);
	CHECK_EQ(ll, -16);

	CHECK_EQ(kstrtouint("4294967295", 10, &ui), 0);
	CHECK(ui == UINT_MAX);
	CHECK_EQ(kstrtouint("4294967296", 10, &ui), -ERANGE);
	CHECK_EQ(kstrtoint("-2147483648", 10, &i), 0);
	CHECK(i == INT_MIN);
	CHECK_EQ(kstrtoint("2147483648", 10, &i), -ERANGE);
	CHECK_EQ(kstrtoint("-12\n", 0, &i), 0);
	CHECK_EQ(i, -12);
	CHECK_EQ(kstrtou16("65535", 0, &u16v), 0);
	CHECK_EQ(u16v, 65535);
	CHECK_EQ(kstrtou16("65536", 0, &u16v), -ERANGE);
	CHECK_EQ(kstrtou8("0xff", 0, &u8v), 0);
	CHECK_EQ(u8v, 255);
	CHECK_EQ(kstrtou8("256", 0, &u8v), -ERANGE);
	CHECK_EQ(kstrtos8("-128", 0, &s8v), 0);
	CHECK_EQ(s8v, -128);
	CHECK_EQ(kstrtos8("128", 0, &s8v), -ERANGE);
	CHECK_EQ(kstrtoul("42", 10, &ul), 0);
	CHECK_EQ(ul, 42);
	CHECK_EQ(kstrtol("-42", 10, &l), 0);
	CHECK_EQ(l, -42);

	CHECK_EQ(kstrtobool("1", &b), 0);
	CHECK(b);
	CHECK_EQ(kstrtobool("N", &b), 0);
	CHECK(!b);
	CHECK_EQ(kstrtobool("on", &b), 0);
	CHECK(b);
	CHECK_EQ(kstrtobool("off", &b), 0);
	CHECK(!b);
	CHECK_EQ(kstrtobool("yes", &b), 0);
	CHECK(b);
	CHECK_EQ(kstrtobool("o", &b), -EINVAL);
	CHECK_EQ(kstrtobool("x", &b), -EINVAL);
	CHECK_EQ(kstrtobool(NULL, &b), -EINVAL);

	{
		char *end;

		CHECK_EQ(simple_strtoul("123abc", &end, 10), 123);
		CHECK_STR(end, "abc");
		CHECK_EQ(simple_strtoul("0x1Gz", &end, 0), 1);
		CHECK_STR(end, "Gz");
		CHECK_EQ(simple_strtol("-77k", &end, 0), -77);
		CHECK_STR(end, "k");
		CHECK_EQ(simple_strtoull("0755", NULL, 0), 0755);
	}
}

static const char *const names[] = { "zero", "one", "two" };

TEST(string_helpers)
{
	char b[16], *p, *s, *tok;
	char pad[8];
	u32 w[4];
	u64 q[3];
	char *dup;
	void *m;

	CHECK_EQ(strnlen("hello", 3), 3);
	CHECK_EQ(strnlen("hi", 30), 2);
	CHECK(memchr("abcdef", 'd', 6) != NULL);
	CHECK(memchr("abcdef", 'd', 3) == NULL);
	memset(b, 7, sizeof (b));
	CHECK(memchr_inv(b, 7, sizeof (b)) == NULL);
	b[9] = 8;
	CHECK(memchr_inv(b, 7, sizeof (b)) == b + 9);

	CHECK_EQ(strscpy(b, "short"), 5);
	CHECK_STR(b, "short");
	CHECK_EQ(strscpy(b, "0123456789abcdefXYZ"), -E2BIG);
	CHECK_STR(b, "0123456789abcde");
	CHECK_EQ(strscpy(b, "abc", 2), -E2BIG);
	CHECK_STR(b, "a");
	CHECK_EQ(sized_strscpy(b, "abc", 0), -E2BIG);
	memset(pad, 'x', sizeof (pad));
	CHECK_EQ(strscpy_pad(pad, "ab", sizeof (pad)), 2);
	CHECK(pad[2] == 0 && pad[7] == 0);

	dup = kstrdup("dup", GFP_KERNEL);
	CHECK_STR(dup, "dup");
	kfree(dup);
	CHECK(kstrdup(NULL, GFP_KERNEL) == NULL);
	dup = kstrndup("abcdef", 3, GFP_KERNEL);
	CHECK_STR(dup, "abc");
	kfree(dup);
	dup = kmemdup_nul("xyz123", 3, GFP_KERNEL);
	CHECK_STR(dup, "xyz");
	kfree(dup);
	kfree_const(kstrdup_const("c", GFP_KERNEL));
	m = kmemdup_array(w, 2, sizeof (u32), GFP_KERNEL);
	CHECK(m != NULL);
	kfree(m);
	CHECK(kmemdup_array(w, SIZE_MAX / 2, 4, GFP_KERNEL) == NULL);

	strcpy(b, "  pad me \n");
	CHECK_STR(strim(b), "pad me");
	CHECK_STR(skip_spaces("\t x"), "x");
	strcpy(b, "a-b-c");
	CHECK(strreplace(b, '-', '_') == b);
	CHECK_STR(b, "a_b_c");
	strcpy(b, "x,y;;z");
	s = b;
	tok = strsep(&s, ",;");
	CHECK_STR(tok, "x");
	tok = strsep(&s, ",;");
	CHECK_STR(tok, "y");
	tok = strsep(&s, ",;");
	CHECK_STR(tok, "");
	tok = strsep(&s, ",;");
	CHECK_STR(tok, "z");
	CHECK(s == NULL);
	CHECK(strsep(&s, ",") == NULL);
	CHECK_STR(strnstr("hello world", "wor", 11), "world");
	CHECK(strnstr("hello world", "wor", 8) == NULL);
	CHECK_STR(strnstr("abc", "", 0), "abc");
	CHECK_STR(strnchr("abcabc", 6, 'c'), "cabc");
	CHECK(strnchr("abcabc", 2, 'c') == NULL);
	p = strchrnul("abc", 'z');
	CHECK(*p == '\0');
	CHECK(sysfs_streq("on", "on\n"));
	CHECK(sysfs_streq("on\n", "on"));
	CHECK(!sysfs_streq("on", "o"));
	CHECK(!sysfs_streq("on", "on\n\n"));
	CHECK_EQ(match_string(names, ARRAY_SIZE(names), "two"), 2);
	CHECK_EQ(match_string(names, ARRAY_SIZE(names), "six"), -EINVAL);
	CHECK(memset32(w, 0xabcd0123, 4) == w);
	CHECK(w[0] == 0xabcd0123 && w[3] == 0xabcd0123);
	memset64(q, 0x1122334455667788ULL, 3);
	CHECK(q[2] == 0x1122334455667788ULL);
}

TEST(hex)
{
	u8 bin[4];
	char hex[9];

	CHECK_EQ(hex_to_bin('a'), 10);
	CHECK_EQ(hex_to_bin('F'), 15);
	CHECK_EQ(hex_to_bin('9'), 9);
	CHECK_EQ(hex_to_bin('g'), -1);
	CHECK_EQ(hex2bin(bin, "deadBEEF", 4), 0);
	CHECK(bin[0] == 0xde && bin[3] == 0xef);
	CHECK_EQ(hex2bin(bin, "dexd", 2), -EINVAL);
	*bin2hex(hex, bin, 4) = '\0';
	CHECK_STR(hex, "deadbeef");
	CHECK(hex_asc[11] == 'b' && hex_asc_upper[11] == 'B');
}

TEST(string_get_size)
{
	char b[32];

	string_get_size(512, 1, STRING_UNITS_2, b, sizeof (b));
	CHECK_STR(b, "512 B");
	string_get_size(4, 1024 * 1024, STRING_UNITS_2, b, sizeof (b));
	CHECK_STR(b, "4.00 MiB");
	string_get_size(10000, 1, STRING_UNITS_2, b, sizeof (b));
	CHECK_STR(b, "9.77 KiB");
	string_get_size(10000, 1, STRING_UNITS_10, b, sizeof (b));
	CHECK_STR(b, "10.0 kB");
	string_get_size(123456789, 1, STRING_UNITS_10, b, sizeof (b));
	CHECK_STR(b, "123 MB");
	string_get_size(8ULL << 30, 1, STRING_UNITS_2, b, sizeof (b));
	CHECK_STR(b, "8.00 GiB");
	string_get_size(1ULL << 62, 512, STRING_UNITS_2, b, sizeof (b));
	CHECK_STR(b, "2.00 ZiB");
	string_get_size(3, 0, STRING_UNITS_10, b, sizeof (b));
	CHECK_STR(b, "0 B");
	string_get_size(1536, 1, STRING_UNITS_2 | STRING_UNITS_NO_SPACE, b,
	    sizeof (b));
	CHECK_STR(b, "1.50KiB");
	CHECK_EQ(string_get_size(1536, 1, STRING_UNITS_2, b, 4), 8);
	CHECK_STR(b, "1.5");
}

TEST(sscanf)
{
	int a = 0, b2 = 0, n = 0;
	unsigned int u = 0;
	unsigned short h[8];
	unsigned long long ull = 0;
	unsigned char hh = 0;
	char s[16], c[3] = { 0, 0, 0 };
	long l = 0;

	CHECK_EQ(sscanf("12 -34", "%d %d", &a, &b2), 2);
	CHECK(a == 12 && b2 == -34);
	CHECK_EQ(sscanf("x=0x1f,y=077", "x=%x,y=%o", &u, &a), 2);
	CHECK(u == 31 && a == 63);
	CHECK_EQ(sscanf("0x10 010 10", "%i %i %i", &a, &b2, &n), 3);
	CHECK(a == 16 && b2 == 8 && n == 10);
	CHECK_EQ(sscanf("1 2 3 4 5 6 7 8", "%hu %hu %hu %hu %hu %hu %hu %hu",
	    &h[0], &h[1], &h[2], &h[3], &h[4], &h[5], &h[6], &h[7]), 8);
	CHECK(h[0] == 1 && h[7] == 8);
	CHECK_EQ(sscanf("18446744073709551615", "%llu", &ull), 1);
	CHECK(ull == ULLONG_MAX);
	CHECK_EQ(sscanf("300", "%hhu", &hh), 1);
	CHECK_EQ(hh, 44);
	CHECK_EQ(sscanf("  word rest", "%s%n", s, &n), 1);
	CHECK_STR(s, "word");
	CHECK_EQ(n, 6);
	CHECK_EQ(sscanf("abcdef", "%3s", s), 1);
	CHECK_STR(s, "abc");
	CHECK_EQ(sscanf("xy", "%2c", c), 1);
	CHECK(c[0] == 'x' && c[1] == 'y');
	CHECK_EQ(sscanf("12345", "%2d%3d", &a, &b2), 2);
	CHECK(a == 12 && b2 == 345);
	CHECK_EQ(sscanf("5 skip 6", "%d %*s %d", &a, &b2), 2);
	CHECK(a == 5 && b2 == 6);
	CHECK_EQ(sscanf("7,x", "%d,%d", &a, &b2), 1);
	CHECK_EQ(sscanf("", "%d", &a), 0);
	CHECK_EQ(sscanf("50%", "%d%%", &a), 1);
	CHECK_EQ(sscanf("-9", "%ld", &l), 1);
	CHECK_EQ(l, -9);
	CHECK_EQ(sscanf("abc", "abd%d", &a), 0);
}
