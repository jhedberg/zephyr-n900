/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Character grid of the phone demo's terminal: a VT100 subset, enough for
 * the Zephyr shell. Lines are numbered as they scroll in; the screen is the
 * last TERM_ROWS of them and up to TERM_LINES are kept, the screen included.
 * Writes may come from any thread; readers take the lock.
 */
#ifndef TERM_H_
#define TERM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_COLS  88
#define TERM_ROWS  23
#define TERM_LINES 200

/* Colour of a cell: the default one, or 1 + an ANSI colour 0..7 */
#define TERM_COLOR_DEFAULT 0U

struct term_cell {
	char c;
	uint8_t color;
};

struct term_view {
	/* Oldest line kept, and one past the newest */
	uint32_t first;
	uint32_t end;
	/* Line and column of the cursor */
	uint32_t cursor_line;
	uint16_t cursor_col;
};

/* Sends the terminal's answers to queries, such as the cursor position */
typedef void (*term_reply_t)(const char *data, size_t len);

void term_init(term_reply_t reply);
void term_write(const char *data, size_t len);

/* True once after every change */
bool term_take_changed(void);

/* The rest is called with the lock held */
void term_lock(struct term_view *view);
void term_unlock(void);
const struct term_cell *term_line(uint32_t line);
bool term_take_dirty(uint32_t line);

/*
 * The shell on the terminal (shell_term.c): typed input for the shell, and
 * the acknowledgment of a PEV_TERM event, which is posted once until then.
 */
void term_shell_input(const char *data, size_t len);
void term_shell_ack(void);

#endif
