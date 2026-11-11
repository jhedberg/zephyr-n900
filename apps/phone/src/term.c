/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The VT100 subset of the Zephyr shell: CR, LF, BS and TAB, cursor movement
 * (CUU, CUD, CUF, CUB, CUP, CHA, VPA), erasing (ED, EL), SGR foreground
 * colours, saving and restoring the cursor, the autowrap mode and the cursor
 * position report the shell's resize command asks for. Like a VT100, a
 * character written to the last column leaves the cursor there and the next
 * one wraps, as the shell moves to a new line itself when a command line
 * fills one.
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "term.h"

#define MAX_PARAMS 4
#define MAX_PARAM  9999U

enum parse_state {
	PARSE_TEXT,
	PARSE_ESC,
	PARSE_CSI,
	/* The character set designator after ESC ( or ESC ) */
	PARSE_SKIP,
	/* The continuation bytes of a UTF-8 sequence */
	PARSE_UTF8,
};

static K_MUTEX_DEFINE(lock);
static atomic_t changed;
static term_reply_t reply_cb;

static struct term_cell cells[TERM_LINES][TERM_COLS];
static bool dirty[TERM_LINES];
static uint32_t end;

/* On the screen; col is TERM_COLS while a wrap is pending */
static uint16_t row;
static uint16_t col;
static uint8_t color;
static uint16_t saved_row;
static uint16_t saved_col;
static uint8_t saved_color;
static bool autowrap;

static enum parse_state state;
static uint16_t params[MAX_PARAMS];
static uint8_t param_count;
static bool csi_private;
static uint8_t utf8_left;

static struct term_cell *line_cells(uint32_t line)
{
	return cells[line % TERM_LINES];
}

static uint32_t screen_line(uint16_t r)
{
	return end - TERM_ROWS + r;
}

static void erase(uint32_t line, uint16_t from, uint16_t to)
{
	struct term_cell *c = line_cells(line);

	for (uint16_t i = from; i < MIN(to, TERM_COLS); i++) {
		c[i].c = ' ';
		c[i].color = TERM_COLOR_DEFAULT;
	}
	dirty[line % TERM_LINES] = true;
}

static void erase_screen(uint16_t from_row, uint16_t to_row)
{
	for (uint16_t r = from_row; r < to_row; r++) {
		erase(screen_line(r), 0, TERM_COLS);
	}
}

static void line_feed(void)
{
	if ((row + 1U) < TERM_ROWS) {
		row++;
		return;
	}

	/* The new line takes the place of the oldest one */
	end++;
	erase(end - 1U, 0, TERM_COLS);
}

/* A pending wrap ends when the cursor moves */
static void settle(void)
{
	if (col >= TERM_COLS) {
		col = TERM_COLS - 1U;
	}
}

static void put(char ch)
{
	struct term_cell *c;

	if (col >= TERM_COLS) {
		if (!autowrap) {
			col = TERM_COLS - 1U;
		} else {
			col = 0U;
			line_feed();
		}
	}

	c = &line_cells(screen_line(row))[col];
	c->c = ch;
	c->color = color;
	dirty[screen_line(row) % TERM_LINES] = true;
	col++;
}

static void control(char ch)
{
	switch (ch) {
	case '\r':
		col = 0U;
		break;
	case '\n':
	case '\v':
	case '\f':
		settle();
		line_feed();
		break;
	case '\b':
		settle();
		if (col > 0U) {
			col--;
		}
		break;
	case '\t':
		settle();
		col = MIN(ROUND_DOWN(col + 8U, 8U), TERM_COLS - 1U);
		break;
	case '\x1b':
		state = PARSE_ESC;
		break;
	default:
		/* BEL and the rest have no effect */
		break;
	}
}

static uint16_t param(uint8_t i, uint16_t def)
{
	return ((i < param_count) && (params[i] != 0U)) ? params[i] : def;
}

static void reply(const char *fmt, int a, int b)
{
	char buf[16];
	int len = snprintf(buf, sizeof(buf), fmt, a, b);

	if ((reply_cb != NULL) && (len > 0)) {
		reply_cb(buf, MIN((size_t)len, sizeof(buf) - 1U));
	}
}

static void sgr(void)
{
	if (param_count == 0U) {
		color = TERM_COLOR_DEFAULT;
		return;
	}

	for (uint8_t i = 0U; i < param_count; i++) {
		uint16_t p = params[i];

		if (p == 0U || p == 39U) {
			color = TERM_COLOR_DEFAULT;
		} else if (IN_RANGE(p, 30U, 37U)) {
			color = (uint8_t)(1U + p - 30U);
		} else if (IN_RANGE(p, 90U, 97U)) {
			color = (uint8_t)(1U + p - 90U);
		} else {
			/* Bold, underline and backgrounds are not shown */
		}
	}
}

static void csi(char final)
{
	uint16_t n = param(0, 1);

	if (final != 'm' && final != 'n' && final != 'h' && final != 'l') {
		settle();
	}

	switch (final) {
	case 'A':
		row = (row > n) ? (row - n) : 0U;
		break;
	case 'B':
		row = MIN(row + n, TERM_ROWS - 1U);
		break;
	case 'C':
		col = MIN(col + n, TERM_COLS - 1U);
		break;
	case 'D':
		col = (col > n) ? (col - n) : 0U;
		break;
	case 'G':
		col = MIN(n, TERM_COLS) - 1U;
		break;
	case 'd':
		row = MIN(n, TERM_ROWS) - 1U;
		break;
	case 'H':
	case 'f':
		row = MIN(n, TERM_ROWS) - 1U;
		col = MIN(param(1, 1), TERM_COLS) - 1U;
		break;
	case 'J':
		switch (param(0, 0)) {
		case 0:
			erase(screen_line(row), col, TERM_COLS);
			erase_screen(row + 1U, TERM_ROWS);
			break;
		case 1:
			erase_screen(0U, row);
			erase(screen_line(row), 0U, col + 1U);
			break;
		default:
			erase_screen(0U, TERM_ROWS);
			break;
		}
		break;
	case 'K':
		switch (param(0, 0)) {
		case 0:
			erase(screen_line(row), col, TERM_COLS);
			break;
		case 1:
			erase(screen_line(row), 0U, col + 1U);
			break;
		default:
			erase(screen_line(row), 0U, TERM_COLS);
			break;
		}
		break;
	case 'm':
		sgr();
		break;
	case 'n':
		if (param(0, 0) == 6U) {
			reply("\x1b[%d;%dR", row + 1, MIN(col, TERM_COLS - 1U) + 1);
		} else if (param(0, 0) == 5U) {
			reply("\x1b[0n", 0, 0);
		}
		break;
	case 'h':
	case 'l':
		if (csi_private && (param(0, 0) == 7U)) {
			autowrap = (final == 'h');
		}
		break;
	default:
		break;
	}
}

static void escape(char ch)
{
	state = PARSE_TEXT;

	switch (ch) {
	case '[':
		state = PARSE_CSI;
		param_count = 0U;
		csi_private = false;
		memset(params, 0, sizeof(params));
		break;
	case '(':
	case ')':
		state = PARSE_SKIP;
		break;
	case '7':
		saved_row = row;
		saved_col = col;
		saved_color = color;
		break;
	case '8':
		row = saved_row;
		col = saved_col;
		color = saved_color;
		break;
	case 'D':
		settle();
		line_feed();
		break;
	case 'E':
		col = 0U;
		line_feed();
		break;
	case 'M':
		settle();
		if (row > 0U) {
			row--;
		}
		break;
	case 'c':
		color = TERM_COLOR_DEFAULT;
		autowrap = true;
		row = 0U;
		col = 0U;
		erase_screen(0U, TERM_ROWS);
		break;
	default:
		break;
	}
}

static void csi_byte(char ch)
{
	if ((ch >= '0') && (ch <= '9')) {
		if (param_count == 0U) {
			param_count = 1U;
		}
		if (param_count <= MAX_PARAMS) {
			uint16_t *p = &params[param_count - 1U];
			uint32_t v = (uint32_t)*p * 10U + (uint32_t)(ch - '0');

			*p = (uint16_t)MIN(v, MAX_PARAM);
		}
	} else if (ch == ';') {
		if (param_count == 0U) {
			param_count = 1U;
		}
		if (param_count < MAX_PARAMS) {
			param_count++;
		}
	} else if (ch == '?') {
		csi_private = true;
	} else if ((ch >= 0x40) && (ch <= 0x7E)) {
		state = PARSE_TEXT;
		param_count = MIN(param_count, MAX_PARAMS);
		csi(ch);
	} else if ((uint8_t)ch < 0x20U) {
		control(ch);
	} else {
		/* Intermediate bytes are not used */
	}
}

static void feed(char ch)
{
	uint8_t b = (uint8_t)ch;

	switch (state) {
	case PARSE_ESC:
		escape(ch);
		return;
	case PARSE_CSI:
		csi_byte(ch);
		return;
	case PARSE_SKIP:
		state = PARSE_TEXT;
		return;
	case PARSE_UTF8:
		if ((b & 0xC0U) == 0x80U) {
			if (--utf8_left == 0U) {
				state = PARSE_TEXT;
			}
			return;
		}
		state = PARSE_TEXT;
		break;
	default:
		break;
	}

	if (b < 0x20U) {
		control(ch);
	} else if (b < 0x7FU) {
		put(ch);
	} else if (b >= 0xC0U) {
		/* The font is ASCII: one '?' for each other character */
		put('?');
		utf8_left = (b >= 0xF0U) ? 3U : ((b >= 0xE0U) ? 2U : 1U);
		state = PARSE_UTF8;
	} else {
		/* DEL and stray continuation bytes */
	}
}

void term_init(term_reply_t reply_fn)
{
	k_mutex_lock(&lock, K_FOREVER);
	reply_cb = reply_fn;
	end = TERM_ROWS;
	row = 0U;
	col = 0U;
	color = TERM_COLOR_DEFAULT;
	autowrap = true;
	state = PARSE_TEXT;
	for (uint32_t line = 0U; line < TERM_LINES; line++) {
		erase(line, 0U, TERM_COLS);
	}
	k_mutex_unlock(&lock);
	atomic_set(&changed, 1);
}

void term_write(const char *data, size_t len)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0U; i < len; i++) {
		feed(data[i]);
	}
	k_mutex_unlock(&lock);
	atomic_set(&changed, 1);
}

bool term_take_changed(void)
{
	return atomic_clear(&changed) != 0;
}

void term_lock(struct term_view *view)
{
	k_mutex_lock(&lock, K_FOREVER);
	view->first = (end > TERM_LINES) ? (end - TERM_LINES) : 0U;
	view->end = end;
	view->cursor_line = screen_line(row);
	view->cursor_col = MIN(col, TERM_COLS - 1U);
}

void term_unlock(void)
{
	k_mutex_unlock(&lock);
}

const struct term_cell *term_line(uint32_t line)
{
	return line_cells(line);
}

bool term_take_dirty(uint32_t line)
{
	bool was = dirty[line % TERM_LINES];

	dirty[line % TERM_LINES] = false;
	return was;
}
