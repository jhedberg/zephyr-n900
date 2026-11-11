/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/* The phone demo's terminal grid, fed what the Zephyr shell writes */
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "term.h"

static char reply_buf[32];
static size_t reply_len;

static void reply(const char *data, size_t len)
{
	len = MIN(len, sizeof(reply_buf) - 1U - reply_len);
	memcpy(&reply_buf[reply_len], data, len);
	reply_len += len;
	reply_buf[reply_len] = '\0';
}

static void put(const char *s)
{
	term_write(s, strlen(s));
}

/* A line's text without its trailing spaces */
static const char *text(uint32_t line)
{
	static char buf[TERM_COLS + 1];
	struct term_view v;
	const struct term_cell *c;
	size_t len = TERM_COLS;

	term_lock(&v);
	c = term_line(line);
	for (size_t i = 0; i < TERM_COLS; i++) {
		buf[i] = c[i].c;
	}
	term_unlock();

	while ((len > 0U) && (buf[len - 1U] == ' ')) {
		len--;
	}
	buf[len] = '\0';

	return buf;
}

static struct term_view view(void)
{
	struct term_view v;

	term_lock(&v);
	term_unlock();

	return v;
}

/* The line of a screen row */
static uint32_t row(uint16_t r)
{
	return view().end - TERM_ROWS + r;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	reply_len = 0U;
	reply_buf[0] = '\0';
	term_init(reply);
}

ZTEST(term, test_text_and_newlines)
{
	struct term_view v;

	put("n900:~$ help\r\nAvailable commands:\r\n");

	v = view();
	zassert_str_equal(text(row(0)), "n900:~$ help");
	zassert_str_equal(text(row(1)), "Available commands:");
	zassert_equal(v.cursor_line, row(2));
	zassert_equal(v.cursor_col, 0);
	zassert_true(term_take_changed());
	zassert_false(term_take_changed());
}

/* A full line leaves the cursor in its last column until the next character */
ZTEST(term, test_deferred_wrap)
{
	char line[TERM_COLS + 1];
	struct term_view v;

	memset(line, 'x', TERM_COLS);
	line[TERM_COLS] = '\0';

	put(line);
	v = view();
	zassert_equal(v.cursor_line, row(0));
	zassert_equal(v.cursor_col, TERM_COLS - 1U);

	/* As the shell moves to a new line itself, that adds no empty line */
	put("\r\nnext");
	zassert_str_equal(text(row(1)), "next");

	put("\r\n");
	put(line);
	put("y");
	zassert_str_equal(text(row(2)), line);
	zassert_str_equal(text(row(3)), "y");
}

ZTEST(term, test_scrollback)
{
	char buf[16];
	struct term_view v;

	for (int i = 0; i < TERM_ROWS + 5; i++) {
		snprintf(buf, sizeof(buf), "line %d\r\n", i);
		put(buf);
	}

	v = view();
	zassert_equal(v.end, 2U * TERM_ROWS + 5U - (TERM_ROWS - 1U));
	zassert_str_equal(text(0), "line 0");
	zassert_str_equal(text(row(TERM_ROWS - 2U)), "line 27");
	zassert_equal(v.first, 0U);

	for (int i = 0; i < TERM_LINES; i++) {
		put("more\r\n");
	}

	v = view();
	zassert_equal(v.first, v.end - TERM_LINES);
	zassert_str_equal(text(v.first), "more");
}

ZTEST(term, test_cursor_and_erase)
{
	struct term_view v;

	put("abcdef\x1b[3D");
	v = view();
	zassert_equal(v.cursor_col, 3);

	put("\x1b[K");
	zassert_str_equal(text(row(0)), "abc");

	put("\r\nsecond\x1b[1A\x1b[3D!");
	zassert_str_equal(text(row(0)), "abc!");

	put("\x1b[2;3HX");
	zassert_str_equal(text(row(1)), "seXond");

	/* Erase from the cursor to the end of the screen, as the shell does */
	put("\x1b[1;2H\x1b[J");
	zassert_str_equal(text(row(0)), "a");
	zassert_str_equal(text(row(1)), "");

	put("\x1b[2J\x1b[H");
	v = view();
	zassert_str_equal(text(row(0)), "");
	zassert_equal(v.cursor_line, row(0));
	zassert_equal(v.cursor_col, 0);
}

ZTEST(term, test_backspace_and_tab)
{
	put("abc\b\bX\tY");
	zassert_str_equal(text(row(0)), "aXc     Y");
}

ZTEST(term, test_save_restore)
{
	/* ESC 7 and ESC 8, in octal so that the digits are not taken as hex */
	put("ab\0337cd\0338X");
	zassert_str_equal(text(row(0)), "abXd");
}

ZTEST(term, test_colors)
{
	struct term_view v;
	const struct term_cell *c;

	put("\x1b[1;31merr\x1b[m ok \x1b[32mgo\x1b[0m");

	term_lock(&v);
	c = term_line(row(0));
	zassert_equal(c[0].color, 1U + 1U);
	zassert_equal(c[2].color, 1U + 1U);
	zassert_equal(c[3].color, TERM_COLOR_DEFAULT);
	zassert_equal(c[4].color, TERM_COLOR_DEFAULT);
	zassert_equal(c[7].color, 1U + 2U);
	term_unlock();
}

ZTEST(term, test_dirty)
{
	struct term_view v;

	term_lock(&v);
	for (uint32_t line = v.first; line < v.end; line++) {
		(void)term_take_dirty(line);
	}
	term_unlock();

	put("\x1b[3;1Hz");

	term_lock(&v);
	zassert_false(term_take_dirty(row(1)));
	zassert_true(term_take_dirty(row(2)));
	zassert_false(term_take_dirty(row(2)));
	term_unlock();
}

ZTEST(term, test_cursor_position_report)
{
	put("\x1b[5;7H\x1b[6n");
	zassert_str_equal(reply_buf, "\x1b[5;7R");
}

/* Characters beyond ASCII show as one '?' each */
ZTEST(term, test_utf8)
{
	put("\xc2\xb5s \xe2\x82\xac!");
	zassert_str_equal(text(row(0)), "?s ?!");
}

ZTEST(term, test_autowrap_off)
{
	char line[TERM_COLS + 1];

	memset(line, 'x', TERM_COLS);
	line[TERM_COLS] = '\0';

	put("\x1b[?7l");
	put(line);
	put("yz");
	zassert_equal(text(row(0))[TERM_COLS - 1], 'z');
	zassert_str_equal(text(row(1)), "");
}

ZTEST_SUITE(term, NULL, NULL, before, NULL, NULL);
