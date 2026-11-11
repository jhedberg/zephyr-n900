/* N900 phone demo: LVGL user interface */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/input/input.h>
#include <zephyr/version.h>
#include <lvgl.h>

#include "camera.h"
#include "kites.h"
#include "phone.h"
#include "sound.h"
#include "term.h"

LV_FONT_DECLARE(lv_font_adwaita_mono_14);

/* Current resolution, which the rotation swaps */
#define SCR_W    lv_display_get_horizontal_resolution(NULL)
#define SCR_H    lv_display_get_vertical_resolution(NULL)
#define PORTRAIT (SCR_W < SCR_H)
#define BAR_H 44

#define COL_BG     lv_color_hex(0x101418)
#define COL_BAR    lv_color_hex(0x000000)
#define COL_TILE   lv_color_hex(0x1E2A36)
#define COL_ACCENT lv_color_hex(0x3DA5FF)
#define COL_GREEN  lv_color_hex(0x2ECC71)
#define COL_RED    lv_color_hex(0xE74C3C)
#define COL_TEXT   lv_color_hex(0xF0F0F0)
#define COL_DIM    lv_color_hex(0x8899AA)

enum app_id {
	APP_HOME,
	APP_PHONE,
	APP_MESSAGES,
	APP_CAMERA,
	APP_SENSORS,
	APP_SETTINGS,
	APP_TORCH,
	APP_TERMINAL,
};

enum call_state {
	CALL_NONE,
	CALL_DIALING,
	CALL_RINGING, /* incoming */
	CALL_ACTIVE,
};

/* System state */
static struct {
	enum app_id app;
	bool screen_on;
	bool locked;
	bool slide_open;
	bool lens_closed;
	bool near;
	bool cover_closed;
	bool torch;
	bool auto_brightness;
	bool auto_rotate;
	lv_display_rotation_t rotation;
	lv_display_rotation_t rot_candidate;
	int rot_ticks;
	uint8_t brightness; /* 10..255 */
	uint8_t kbd_level;  /* 0..100 */
	bool shift;
	bool fn;
	int64_t power_down_at;
	int unread;
	int taps;
	int photos;
	enum call_state call;
	int64_t call_start;
	char call_number[24];
	char call_name[24];
	int64_t led_test_until;
	uint8_t led_test_rgb[3];
	int64_t flash_until;
	uint8_t volume; /* percent */
	bool key_sounds;
	struct phone_sensors s;
} st = {
	.screen_on = true,
	.brightness = 200,
	.auto_rotate = true,
	.kbd_level = 60,
	.lens_closed = true,
	.volume = 70,
	.key_sounds = true,
	.cover_closed = true,
};

/* Persistent widgets */
static lv_obj_t *bar_title;
static lv_obj_t *bar_clock;
static lv_obj_t *bar_status;
static lv_obj_t *bar_home;
static lv_obj_t *bar_kite;
static lv_obj_t *content;
static lv_obj_t *toast;
static lv_obj_t *lock_layer;
static lv_obj_t *lock_clock;
static lv_obj_t *call_layer;
static lv_obj_t *flash_layer;

/* Per-app hooks */
static void (*app_tick)(void);
static void (*app_char)(char c);
static void (*app_key)(uint16_t code, bool pressed);

static void open_app(enum app_id app);
static void rotation_update(void);

/* ---------- helpers ---------- */

static int build_secs;

static void clock_text(char *buf, size_t len, bool secs)
{
	int t = build_secs + (int)(k_uptime_get() / 1000);

	if (secs) {
		snprintf(buf, len, "%02d:%02d:%02d", (t / 3600) % 24, (t / 60) % 60, t % 60);
	} else {
		snprintf(buf, len, "%02d:%02d", (t / 3600) % 24, (t / 60) % 60);
	}
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
		       const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_text(l, text);
	return l;
}

static lv_obj_t *panel(lv_obj_t *parent, int32_t w, int32_t h)
{
	lv_obj_t *p = lv_obj_create(parent);

	lv_obj_set_size(p, w, h);
	lv_obj_set_style_bg_color(p, COL_TILE, 0);
	lv_obj_set_style_border_width(p, 0, 0);
	lv_obj_set_style_radius(p, 14, 0);
	lv_obj_set_style_pad_all(p, 12, 0);
	lv_obj_set_scrollable(p, false);
	return p;
}

static void click_cb(lv_event_t *e)
{
	if (st.key_sounds) {
		sound_play(SOUND_CLICK);
	}
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb,
			void *user)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = lv_label_create(b);

	lv_obj_set_style_bg_color(b, color, 0);
	lv_obj_set_style_radius(b, 12, 0);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	if (cb != NULL) {
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	}
	lv_obj_add_event_cb(b, click_cb, LV_EVENT_PRESSED, NULL);
	return b;
}

static lv_timer_t *toast_timer;

static void toast_hide(lv_timer_t *t)
{
	lv_obj_set_hidden(toast, true);
	lv_timer_pause(t);
}

static void show_toast(const char *text)
{
	lv_label_set_text(lv_obj_get_child(toast, 0), text);
	lv_obj_set_hidden(toast, false);
	lv_obj_move_foreground(toast);
	lv_timer_reset(toast_timer);
	lv_timer_resume(toast_timer);
}

/* ---------- screen, lock, LEDs ---------- */

static void screen_set(bool on)
{
	if (on == st.screen_on) {
		return;
	}
	st.screen_on = on;
	hw_screen(on, st.brightness);
	if (on) {
		lv_obj_invalidate(lv_screen_active());
	}
}

static void set_brightness(uint8_t b)
{
	st.brightness = CLAMP(b, 10, 255);
	if (st.screen_on) {
		hw_screen(true, st.brightness);
	}
}

static void lock_set(bool locked)
{
	st.locked = locked;
	if (locked) {
		lv_obj_set_hidden(lock_layer, false);
		screen_set(false);
	} else {
		lv_obj_set_hidden(lock_layer, true);
		screen_set(true);
	}
}

static void leds_update(void)
{
	int64_t now = k_uptime_get();
	int phase = (int)((now / 100) % 20);
	uint8_t r = 0, g = 0, b = 0;

	if (st.torch || (now < st.flash_until)) {
		hw_status_led(100, 100, 100);
		hw_kbd_backlight(100);
		return;
	}

	hw_kbd_backlight((st.slide_open && st.screen_on) ? st.kbd_level : 0);

	if (now < st.led_test_until) {
		r = st.led_test_rgb[0];
		g = st.led_test_rgb[1];
		b = st.led_test_rgb[2];
	} else if (st.call == CALL_RINGING) {
		b = ((phase % 5) < 2) ? 100 : 0;
	} else if (st.unread > 0) {
		g = (phase < 2) ? 100 : 0;
	} else if (st.s.charger == CHARGER_STATUS_CHARGING) {
		r = 15;
		g = 3;
	} else if (st.s.charger == CHARGER_STATUS_FULL) {
		g = 15;
	}
	hw_status_led(r, g, b);
}

/* ---------- status bar ---------- */

static void bar_update(void)
{
	char buf[64];
	const char *batt;
	int soc = st.s.soc;

	clock_text(buf, sizeof(buf), false);
	lv_label_set_text(bar_clock, buf);

	batt = (soc > 80) ? LV_SYMBOL_BATTERY_FULL
	       : (soc > 55) ? LV_SYMBOL_BATTERY_3
	       : (soc > 30) ? LV_SYMBOL_BATTERY_2
	       : (soc > 10) ? LV_SYMBOL_BATTERY_1
			    : LV_SYMBOL_BATTERY_EMPTY;
	snprintf(buf, sizeof(buf), "%s%s%s%s %d%%", (st.unread > 0) ? LV_SYMBOL_ENVELOPE "  " : "",
		 st.s.vbus ? LV_SYMBOL_USB "  " : "",
		 (st.s.charger == CHARGER_STATUS_CHARGING) ? LV_SYMBOL_CHARGE " " : "", batt,
		 st.s.gauge_ok ? soc : 0);
	lv_label_set_text(bar_status, buf);
}

static void home_cb(lv_event_t *e)
{
	open_app(APP_HOME);
}

/* ---------- home ---------- */

static const struct {
	enum app_id id;
	const char *symbol;
	const char *name;
	uint32_t color;
} apps[] = {
	{APP_PHONE, LV_SYMBOL_CALL, "Phone", 0x27AE60},
	{APP_MESSAGES, LV_SYMBOL_ENVELOPE, "Messages", 0x2E86DE},
	{APP_CAMERA, LV_SYMBOL_IMAGE, "Camera", 0x8E44AD},
	{APP_SENSORS, LV_SYMBOL_GPS, "Sensors", 0xD35400},
	{APP_SETTINGS, LV_SYMBOL_SETTINGS, "Settings", 0x7F8C8D},
	{APP_TORCH, LV_SYMBOL_EYE_OPEN, "Torch", 0xC0A000},
	{APP_TERMINAL, ">_", "Terminal", 0x34495E},
};

static lv_obj_t *home_clock;
static lv_obj_t *home_info;

static void tile_cb(lv_event_t *e)
{
	open_app((enum app_id)(uintptr_t)lv_event_get_user_data(e));
}

static void home_tick(void)
{
	char buf[16];

	clock_text(buf, sizeof(buf), false);
	lv_label_set_text(home_clock, buf);
	lv_label_set_text_fmt(home_info, "Battery %d%%  %s\n%s\nUnread messages: %d",
			      st.s.soc,
			      (st.s.charger == CHARGER_STATUS_CHARGING) ? "charging"
			      : st.s.vbus				  ? "on USB"
									  : "",
			      st.slide_open ? "Keyboard open" : "Keyboard closed", st.unread);
}

/* ---------- kites ---------- */

static void anim_x_cb(void *obj, int32_t v)
{
	lv_obj_set_x(obj, v);
}

static void anim_y_cb(void *obj, int32_t v)
{
	lv_obj_set_y(obj, v);
}

static void anim_opa_cb(void *obj, int32_t v)
{
	lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
		     uint32_t ms, uint32_t delay, bool forever, lv_anim_path_cb_t path)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_delay(&a, delay);
	lv_anim_set_path_cb(&a, path);
	if (forever) {
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	}
	lv_anim_start(&a);
}

/* A kite drifting across the sky, bobbing up and down on the wind */
static void kite_fly(lv_obj_t *sky, const lv_image_dsc_t *img, lv_opa_t opa, int32_t y,
		     uint32_t cross_ms, uint32_t start_ms)
{
	lv_obj_t *k = lv_image_create(sky);
	lv_anim_t a;

	lv_image_set_src(k, img);
	lv_obj_set_style_image_opa(k, opa, 0);
	lv_obj_set_pos(k, -(int32_t)img->header.w, y);
	anim_run(k, anim_x_cb, -(int32_t)img->header.w, SCR_W, cross_ms, start_ms, true,
		 lv_anim_path_linear);

	lv_anim_init(&a);
	lv_anim_set_var(&a, k);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, y, y + 30);
	lv_anim_set_duration(&a, 2500 + cross_ms / 20);
	lv_anim_set_reverse_duration(&a, 2500 + cross_ms / 20);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static lv_obj_t *splash;

static void splash_end(lv_timer_t *t)
{
	if ((splash != NULL) && lv_obj_is_valid(splash)) {
		lv_obj_delete(splash);
	}
	splash = NULL;
}

/* Boot splash: the kite flies in, then the name appears */
static void show_splash(void)
{
	lv_obj_t *kite, *name, *sub;
	lv_timer_t *t;

	splash = lv_obj_create(lv_layer_top());
	lv_obj_set_size(splash, SCR_W, SCR_H);
	lv_obj_set_style_radius(splash, 0, 0);
	lv_obj_set_style_border_width(splash, 0, 0);
	lv_obj_set_style_bg_color(splash, lv_color_hex(0x0B1026), 0);
	lv_obj_set_style_bg_grad_color(splash, lv_color_hex(0x3B1E6E), 0);
	lv_obj_set_style_bg_grad_dir(splash, LV_GRAD_DIR_VER, 0);
	lv_obj_set_scrollable(splash, false);

	kite = lv_image_create(splash);
	lv_image_set_src(kite, &kite_large);
	anim_run(kite, anim_x_cb, -300, (SCR_W - 276) / 2, 1400, 0, false, lv_anim_path_ease_out);
	anim_run(kite, anim_y_cb, SCR_H, SCR_H / 2 - 190, 1400, 0, false, lv_anim_path_ease_out);

	name = label(splash, &lv_font_montserrat_48, COL_TEXT, "Zephyr");
	lv_obj_align(name, LV_ALIGN_CENTER, 0, 50);
	lv_obj_set_style_opa(name, LV_OPA_TRANSP, 0);
	anim_run(name, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 600, 1100, false,
		 lv_anim_path_linear);
	sub = label(splash, &lv_font_montserrat_20, COL_DIM, "RTOS on the Nokia N900");
	lv_obj_align(sub, LV_ALIGN_CENTER, 0, 100);
	lv_obj_set_style_opa(sub, LV_OPA_TRANSP, 0);
	anim_run(sub, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 600, 1400, false,
		 lv_anim_path_linear);

	t = lv_timer_create(splash_end, 3200, NULL);
	lv_timer_set_repeat_count(t, 1);
	sound_play(SOUND_STARTUP);
}

static void build_home(void)
{
	lv_obj_t *left = PORTRAIT ? panel(content, SCR_W - 24, 170)
				  : panel(content, 250, SCR_H - BAR_H - 24);
	lv_obj_t *grid = lv_obj_create(content);
	lv_obj_t *sky = lv_obj_create(content);
	lv_obj_t *z;

	/* Evening sky with kites behind the panels */
	lv_obj_set_size(sky, SCR_W, SCR_H - BAR_H);
	lv_obj_set_style_radius(sky, 0, 0);
	lv_obj_set_style_border_width(sky, 0, 0);
	lv_obj_set_style_pad_all(sky, 0, 0);
	lv_obj_set_style_bg_color(sky, lv_color_hex(0x0E1A3A), 0);
	lv_obj_set_style_bg_grad_color(sky, lv_color_hex(0x2A1450), 0);
	lv_obj_set_style_bg_grad_dir(sky, LV_GRAD_DIR_VER, 0);
	lv_obj_set_scrollable(sky, false);
	lv_obj_move_background(sky);
	kite_fly(sky, &kite_medium, LV_OPA_COVER, 30, 26000, 0);
	kite_fly(sky, &kite_small, LV_OPA_70, (SCR_H - BAR_H) / 2, 38000, 9000);
	kite_fly(sky, &kite_medium, LV_OPA_40, (SCR_H - BAR_H) - 140, 33000, 17000);
	lv_obj_set_style_bg_opa(left, LV_OPA_70, 0);

	lv_obj_align(left, PORTRAIT ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID, PORTRAIT ? 0 : 12,
		     PORTRAIT ? 12 : 0);
	home_clock = label(left, &lv_font_montserrat_48, COL_TEXT, "");
	lv_obj_align(home_clock, LV_ALIGN_TOP_MID, 0, 20);
	z = lv_image_create(left);
	lv_image_set_src(z, &kite_small);
	lv_obj_align(z, LV_ALIGN_TOP_MID, 0, PORTRAIT ? 70 : 84);
	z = label(left, &lv_font_montserrat_20, COL_ACCENT, "Zephyr RTOS");
	lv_obj_align(z, LV_ALIGN_TOP_MID, 0, PORTRAIT ? 104 : 118);
	home_info = label(left, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(home_info, LV_ALIGN_BOTTOM_LEFT, 0, -10);

	if (PORTRAIT) {
		lv_obj_set_size(grid, SCR_W - 24, SCR_H - BAR_H - 206);
		lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -12);
	} else {
		lv_obj_set_size(grid, SCR_W - 290, SCR_H - BAR_H - 24);
		lv_obj_align(grid, LV_ALIGN_RIGHT_MID, -12, 0);
	}
	lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, 0, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_SPACE_EVENLY);
	lv_obj_set_scrollable(grid, false);

	for (size_t i = 0; i < ARRAY_SIZE(apps); i++) {
		lv_obj_t *t = lv_button_create(grid);
		lv_obj_t *sym = label(t, &lv_font_montserrat_48, lv_color_white(), apps[i].symbol);
		lv_obj_t *name = label(t, &lv_font_montserrat_20, lv_color_white(), apps[i].name);

		/* Four to a row in landscape once there are more than six */
		lv_obj_set_size(t, PORTRAIT ? 140 : ((ARRAY_SIZE(apps) > 6U) ? 112 : 150),
				PORTRAIT ? 165 : 175);
		lv_obj_set_style_bg_color(t, lv_color_hex(apps[i].color), 0);
		lv_obj_set_style_radius(t, 22, 0);
		lv_obj_align(sym, LV_ALIGN_CENTER, 0, -18);
		lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
		lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)apps[i].id);
	}

	app_tick = home_tick;
	home_tick();
}

/* ---------- calls ---------- */

static lv_obj_t *call_status;
static lv_obj_t *call_buttons;
static char recents[4][48];

static void add_recent(const char *what)
{
	char t[8];

	clock_text(t, sizeof(t), false);
	memmove(recents[1], recents[0], sizeof(recents) - sizeof(recents[0]));
	snprintf(recents[0], sizeof(recents[0]), "%s  %s", t, what);
}

static void call_end(const char *why)
{
	char buf[48];

	if (st.call == CALL_ACTIVE) {
		int secs = (int)((k_uptime_get() - st.call_start) / 1000);

		snprintf(buf, sizeof(buf), "%s %s (%d:%02d)", LV_SYMBOL_CALL, st.call_name,
			 secs / 60, secs % 60);
		add_recent(buf);
	} else if (why != NULL) {
		snprintf(buf, sizeof(buf), "%s %s %s", LV_SYMBOL_CLOSE, st.call_name, why);
		add_recent(buf);
	}
	st.call = CALL_NONE;
	sound_play(SOUND_CALL_END);
	lv_obj_set_hidden(call_layer, true);
	screen_set(true);
	if (st.app == APP_PHONE) {
		open_app(APP_PHONE);
	}
}

static void hangup_cb(lv_event_t *e)
{
	call_end((st.call == CALL_RINGING) ? "declined" : "cancelled");
}

static void answer_cb(lv_event_t *e);

static void call_buttons_set(bool incoming)
{
	lv_obj_clean(call_buttons);
	if (incoming) {
		lv_obj_t *a = button(call_buttons, LV_SYMBOL_CALL "  Answer", COL_GREEN,
				     answer_cb, NULL);

		lv_obj_set_size(a, PORTRAIT ? 170 : 220, 80);
	}
	lv_obj_t *h = button(call_buttons, LV_SYMBOL_CLOSE "  Hang up", COL_RED, hangup_cb, NULL);

	lv_obj_set_size(h, PORTRAIT ? 170 : 220, 80);
}

static void answer_cb(lv_event_t *e)
{
	sound_stop();
	st.call = CALL_ACTIVE;
	st.call_start = k_uptime_get();
	call_buttons_set(false);
}

static void call_show(const char *name, const char *number, enum call_state state)
{
	strncpy(st.call_name, name, sizeof(st.call_name) - 1);
	strncpy(st.call_number, number, sizeof(st.call_number) - 1);
	st.call = state;
	st.call_start = k_uptime_get();
	lv_label_set_text_fmt(lv_obj_get_child(call_layer, 0), "%s", name);
	lv_label_set_text_fmt(lv_obj_get_child(call_layer, 1), "%s", number);
	call_buttons_set(state == CALL_RINGING);
	sound_play((state == CALL_RINGING) ? SOUND_RINGTONE : SOUND_RINGBACK);
	lv_obj_set_hidden(call_layer, false);
	lv_obj_move_foreground(call_layer);
	screen_set(true);
}

static void call_tick(void)
{
	int64_t el = k_uptime_get() - st.call_start;
	int secs = (int)(el / 1000);

	switch (st.call) {
	case CALL_DIALING:
		lv_label_set_text(call_status, "Calling...");
		if (el > 3500) {
			sound_stop();
			st.call = CALL_ACTIVE;
			st.call_start = k_uptime_get();
		}
		break;
	case CALL_RINGING:
		lv_label_set_text(call_status, "Incoming call");
		if ((el % 1000) < 100) {
			hw_vibrate(400);
		}
		if (el > 20000) {
			call_end("missed");
			show_toast("Missed call");
		}
		break;
	case CALL_ACTIVE:
		lv_label_set_text_fmt(call_status, "%d:%02d  %s", secs / 60, secs % 60,
				      st.near ? "(screen off, near ear)" : "");
		break;
	default:
		break;
	}
}

static void incoming_timer(lv_timer_t *t)
{
	if (st.call == CALL_NONE) {
		call_show("Mom", "+358 40 123 4567", CALL_RINGING);
	}
}

/* ---------- phone app ---------- */

static lv_obj_t *dial_number;
static lv_obj_t *recent_label;
static const char *const keypad_map[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8",
					 "9", "\n", "*", "0", "#", ""};

static void dial_add(char c)
{
	if ((c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+') {
		lv_textarea_add_char(dial_number, c);
		sound_dtmf(c);
	} else if (c == '\b') {
		lv_textarea_delete_char(dial_number);
	} else if (c == '\n') {
		const char *n = lv_textarea_get_text(dial_number);

		if (n[0] != '\0') {
			call_show("Calling", n, CALL_DIALING);
		}
	}
}

static void keypad_cb(lv_event_t *e)
{
	lv_obj_t *m = lv_event_get_target_obj(e);
	const char *t = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));

	if (t != NULL) {
		dial_add(t[0]);
	}
}

static void call_cb(lv_event_t *e)
{
	dial_add('\n');
}

static void del_cb(lv_event_t *e)
{
	dial_add('\b');
}

static void sim_incoming_cb(lv_event_t *e)
{
	lv_timer_t *t = lv_timer_create(incoming_timer, 5000, NULL);

	lv_timer_set_repeat_count(t, 1);
	show_toast("Incoming call in 5 s - go anywhere");
}

static void phone_tick(void)
{
	char buf[200] = "";

	for (size_t i = 0; i < ARRAY_SIZE(recents); i++) {
		if (recents[i][0] != '\0') {
			strcat(buf, recents[i]);
			strcat(buf, "\n");
		}
	}
	lv_label_set_text(recent_label, (buf[0] != '\0') ? buf : "No recent calls");
}

static void build_phone(void)
{
	lv_obj_t *left = PORTRAIT ? panel(content, SCR_W - 24, 420)
				  : panel(content, 400, SCR_H - BAR_H - 24);
	lv_obj_t *right = PORTRAIT ? panel(content, SCR_W - 24, SCR_H - BAR_H - 456)
				   : panel(content, 360, SCR_H - BAR_H - 24);
	lv_obj_t *pad = lv_buttonmatrix_create(left);
	lv_obj_t *b;

	if (PORTRAIT) {
		lv_obj_align(left, LV_ALIGN_TOP_MID, 0, 12);
		lv_obj_align(right, LV_ALIGN_BOTTOM_MID, 0, -12);
	} else {
		lv_obj_align(left, LV_ALIGN_LEFT_MID, 12, 0);
		lv_obj_align(right, LV_ALIGN_RIGHT_MID, -12, 0);
	}

	dial_number = lv_textarea_create(left);
	lv_textarea_set_one_line(dial_number, true);
	lv_textarea_set_placeholder_text(dial_number, "Type or tap a number");
	lv_obj_set_style_text_font(dial_number, &lv_font_montserrat_28, 0);
	lv_obj_set_width(dial_number, lv_pct(100));
	lv_obj_align(dial_number, LV_ALIGN_TOP_MID, 0, 0);

	lv_buttonmatrix_set_map(pad, keypad_map);
	lv_obj_set_size(pad, 250, 280);
	lv_obj_set_style_text_font(pad, &lv_font_montserrat_28, 0);
	lv_obj_align(pad, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_add_event_cb(pad, keypad_cb, LV_EVENT_VALUE_CHANGED, NULL);

	b = button(left, LV_SYMBOL_CALL, COL_GREEN, call_cb, NULL);
	lv_obj_set_size(b, 110, 160);
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, -110);
	b = button(left, LV_SYMBOL_BACKSPACE, COL_RED, del_cb, NULL);
	lv_obj_set_size(b, 110, 100);
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

	lv_obj_t *t = label(right, &lv_font_montserrat_20, COL_TEXT, "Recent calls");
	lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
	recent_label = label(right, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(recent_label, LV_ALIGN_TOP_LEFT, 0, 36);
	b = button(right, LV_SYMBOL_BELL "  Simulate incoming call", COL_ACCENT, sim_incoming_cb,
		   NULL);
	lv_obj_set_size(b, lv_pct(100), 70);
	lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
	t = label(right, &lv_font_montserrat_14, COL_DIM,
		  "During a call the proximity sensor\nturns the screen off.");
	lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, -84);

	app_char = dial_add;
	app_tick = phone_tick;
	phone_tick();
}

/* ---------- messages ---------- */

#define MAX_MSGS 24
static struct {
	bool mine;
	char text[120];
} msgs[MAX_MSGS];
static int msg_count;
static lv_obj_t *chat;
static lv_obj_t *chat_input;

static void chat_add_bubble(int i)
{
	lv_obj_t *row = lv_obj_create(chat);
	lv_obj_t *bubble = lv_obj_create(row);
	lv_obj_t *l = label(bubble, &lv_font_montserrat_20, lv_color_white(), msgs[i].text);

	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 2, 0);
	lv_obj_set_scrollable(row, false);

	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(bubble, SCR_W * 2 / 3, 0);
	lv_obj_set_style_bg_color(bubble, msgs[i].mine ? COL_ACCENT : COL_TILE, 0);
	lv_obj_set_style_border_width(bubble, 0, 0);
	lv_obj_set_style_radius(bubble, 14, 0);
	lv_obj_set_style_pad_all(bubble, 10, 0);
	lv_obj_set_scrollable(bubble, false);
	lv_obj_align(bubble, msgs[i].mine ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_max_width(l, SCR_W * 2 / 3 - 20, 0);
	lv_obj_set_width(l, LV_SIZE_CONTENT);

	lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

static void msg_push(bool mine, const char *text)
{
	if (msg_count == MAX_MSGS) {
		memmove(&msgs[0], &msgs[1], sizeof(msgs[0]) * (MAX_MSGS - 1));
		msg_count--;
		if (chat != NULL) {
			lv_obj_delete(lv_obj_get_child(chat, 0));
		}
	}
	msgs[msg_count].mine = mine;
	strncpy(msgs[msg_count].text, text, sizeof(msgs[0].text) - 1);
	msgs[msg_count].text[sizeof(msgs[0].text) - 1] = '\0';
	msg_count++;
	if (chat != NULL) {
		chat_add_bubble(msg_count - 1);
	}
}

static void bot_answer(const char *q, char *out, size_t len)
{
	char lower[120];
	size_t i;

	for (i = 0; (q[i] != '\0') && (i < sizeof(lower) - 1); i++) {
		lower[i] = (q[i] >= 'A' && q[i] <= 'Z') ? (q[i] - 'A' + 'a') : q[i];
	}
	lower[i] = '\0';

	if (strstr(lower, "batt") != NULL) {
		snprintf(out, len, "Battery %d%%, %d.%02d V, %d mA, %s", st.s.soc, st.s.mv / 1000,
			 (st.s.mv % 1000) / 10, st.s.ma,
			 (st.s.charger == CHARGER_STATUS_CHARGING) ? "charging" : "not charging");
	} else if (strstr(lower, "temp") != NULL) {
		snprintf(out, len, "The battery is at %d.%d C", st.s.temp_dc / 10,
			 abs(st.s.temp_dc % 10));
	} else if ((strstr(lower, "light") != NULL) || (strstr(lower, "lux") != NULL)) {
		snprintf(out, len, "I see %d lux through the front glass", st.s.lux);
	} else if ((strstr(lower, "tilt") != NULL) || (strstr(lower, "accel") != NULL)) {
		snprintf(out, len, "Acceleration x %d.%02d y %d.%02d z %d.%02d m/s2",
			 st.s.ax / 100, abs(st.s.ax % 100), st.s.ay / 100, abs(st.s.ay % 100),
			 st.s.az / 100, abs(st.s.az % 100));
	} else if (strstr(lower, "time") != NULL) {
		char t[12];

		clock_text(t, sizeof(t), true);
		snprintf(out, len, "It is %s", t);
	} else if (strstr(lower, "uptime") != NULL) {
		snprintf(out, len, "Up for %lld s", k_uptime_get() / 1000);
	} else if ((strstr(lower, "hi") == lower) || (strstr(lower, "hello") != NULL)) {
		snprintf(out, len, "Hi! I am Zephyr %s on a Nokia N900 (TI OMAP3430, Cortex-A8).",
			 KERNEL_VERSION_STRING);
	} else {
		snprintf(out, len, "Try: battery, temp, light, tilt, time, uptime");
	}
}

static char pending_question[120];

static void bot_timer(lv_timer_t *t)
{
	char answer[120];

	bot_answer(pending_question, answer, sizeof(answer));
	msg_push(false, answer);
	hw_vibrate(150);
	sound_play(SOUND_MESSAGE);
	if ((st.app != APP_MESSAGES) || !st.screen_on) {
		st.unread++;
		show_toast(LV_SYMBOL_ENVELOPE "  New message from Zephyr Bot");
	}
}

static void chat_send(void)
{
	const char *text = lv_textarea_get_text(chat_input);
	lv_timer_t *t;

	if (text[0] == '\0') {
		return;
	}
	strncpy(pending_question, text, sizeof(pending_question) - 1);
	msg_push(true, text);
	lv_textarea_set_text(chat_input, "");
	t = lv_timer_create(bot_timer, 1500, NULL);
	lv_timer_set_repeat_count(t, 1);
}

static void send_cb(lv_event_t *e)
{
	chat_send();
}

static void chat_char(char c)
{
	if (c == '\n') {
		chat_send();
	} else if (c == '\b') {
		lv_textarea_delete_char(chat_input);
	} else {
		lv_textarea_add_char(chat_input, c);
	}
	if (st.key_sounds) {
		sound_play(SOUND_CLICK);
	}
}

static void build_messages(void)
{
	lv_obj_t *send;

	chat = lv_obj_create(content);
	lv_obj_set_size(chat, SCR_W - 24, SCR_H - BAR_H - 100);
	lv_obj_align(chat, LV_ALIGN_TOP_MID, 0, 8);
	lv_obj_set_style_bg_color(chat, COL_BG, 0);
	lv_obj_set_style_border_width(chat, 0, 0);
	lv_obj_set_flex_flow(chat, LV_FLEX_FLOW_COLUMN);

	chat_input = lv_textarea_create(content);
	lv_textarea_set_one_line(chat_input, true);
	lv_textarea_set_placeholder_text(chat_input,
					 "Slide out the keyboard and type, Enter sends");
	lv_obj_set_size(chat_input, SCR_W - 160, 60);
	lv_obj_align(chat_input, LV_ALIGN_BOTTOM_LEFT, 12, -12);
	lv_obj_add_state(chat_input, LV_STATE_FOCUSED);

	send = button(content, LV_SYMBOL_UPLOAD " Send", COL_ACCENT, send_cb, NULL);
	lv_obj_set_size(send, 120, 60);
	lv_obj_align(send, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

	if (msg_count == 0) {
		msg_push(false, "Hi! I am the Zephyr Bot. Ask me about the battery, the "
				"temperature, the light, the tilt or the time.");
	} else {
		for (int i = 0; i < msg_count; i++) {
			chat_add_bubble(i);
		}
	}
	st.unread = 0;
	app_char = chat_char;
}

/* ---------- camera ---------- */

static lv_obj_t *cam_img;
static lv_obj_t *cam_label;
static lv_obj_t *cam_box;
static lv_obj_t *cam_info;
static lv_obj_t *cam_count;
static lv_obj_t *cam_thumb;
static lv_obj_t *cam_flash_btn;
static lv_obj_t *cam_review;
static lv_timer_t *cam_timer;
static lv_image_dsc_t cam_dsc[2];
static int cam_dsc_next;
static uint32_t cam_seq;
static bool cam_flash_on;
static bool cam_want;
static int cam_shown_photo = -1;
static lv_image_dsc_t cam_photo_dsc;
static lv_image_dsc_t cam_thumb_dsc;

static void cam_dsc_init(lv_image_dsc_t *d, const uint32_t *data, int w, int h)
{
	memset(d, 0, sizeof(*d));
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_XRGB8888;
	d->header.w = w;
	d->header.h = h;
	d->header.stride = w * 4;
	d->data_size = w * h * 4;
	d->data = (const uint8_t *)data;
}

static bool camera_should_run(void)
{
	return (st.app == APP_CAMERA) && !st.lens_closed && st.screen_on && !st.locked;
}

/* Fast refresh of the preview image, separate from the 100 ms UI tick */
static void camera_frame_cb(lv_timer_t *t)
{
	uint32_t seq;
	const uint32_t *buf = cam_preview(&seq);
	lv_image_dsc_t *d;

	if ((buf == NULL) || (seq == cam_seq) || st.lens_closed) {
		return;
	}
	cam_seq = seq;
	d = &cam_dsc[cam_dsc_next];
	cam_dsc_next ^= 1;
	cam_dsc_init(d, buf, CAM_PREVIEW_W, CAM_PREVIEW_H);
	cam_preview_in_use(buf);
	lv_image_set_src(cam_img, d);
	lv_obj_set_hidden(cam_img, false);
}

static void camera_tick(void)
{
	struct cam_status cs;
	lv_color_t box;

	if (camera_should_run() != cam_want) {
		cam_want = !cam_want;
		if (cam_want) {
			cam_start();
		} else {
			cam_stop();
		}
	}
	cam_status_get(&cs);

	if (st.lens_closed) {
		lv_obj_set_hidden(cam_img, true);
		lv_label_set_text(cam_label, "Lens cover closed - slide it open");
	} else if (!cs.running) {
		lv_label_set_text(cam_label, cs.sensor_ok ? "" : "Starting camera...");
	} else if (cs.shooting) {
		lv_label_set_text(cam_label, LV_SYMBOL_IMAGE "  Taking photo");
	} else if (cs.focus == CAM_FOCUS_SCANNING) {
		lv_label_set_text(cam_label, "Focusing...");
	} else if (cs.focus == CAM_FOCUS_LOCKED) {
		lv_label_set_text(cam_label, "Focus locked - press fully to shoot");
	} else {
		lv_label_set_text(cam_label, "Half-press the camera key to focus");
	}

	box = (cs.focus == CAM_FOCUS_LOCKED)   ? COL_GREEN
	      : (cs.focus == CAM_FOCUS_SCANNING) ? lv_color_hex(0xF1C40F)
						 : lv_color_white();
	lv_obj_set_style_border_color(cam_box, box, 0);
	lv_obj_set_hidden(cam_box, !cs.running);

	lv_label_set_text_fmt(cam_info, "%u fps\nexp %u\ngain %u.%ux\nlens %u", cs.fps,
			      cs.exposure, cs.gain_x10 / 10, cs.gain_x10 % 10, cs.lens);
	lv_label_set_text_fmt(cam_count, LV_SYMBOL_IMAGE " %d", cs.photos);

	if ((cs.last_photo >= 0) && (cs.last_photo != cam_shown_photo)) {
		cam_shown_photo = cs.last_photo;
		st.photos = cs.photos;
		cam_dsc_init(&cam_thumb_dsc, cam_photo(cs.last_photo), CAM_PHOTO_W, CAM_PHOTO_H);
		lv_image_set_src(cam_thumb, &cam_thumb_dsc);
		lv_obj_set_hidden(cam_thumb, false);
		show_toast(LV_SYMBOL_IMAGE "  Photo saved");
	}
}

static void flash_off(lv_timer_t *t)
{
	lv_obj_set_hidden(flash_layer, true);
}

static void camera_shoot(void)
{
	lv_timer_t *t;
	struct cam_status cs;

	if (st.lens_closed) {
		show_toast("Open the lens cover first");
		return;
	}
	cam_status_get(&cs);
	if (!cs.running || cs.shooting) {
		return;
	}
	cam_shoot(cam_flash_on);
	hw_vibrate(40);
	sound_play(SOUND_SHUTTER);
	/* Shutter blink */
	lv_obj_set_hidden(flash_layer, false);
	lv_obj_move_foreground(flash_layer);
	t = lv_timer_create(flash_off, 120, NULL);
	lv_timer_set_repeat_count(t, 1);
}

static void camera_key(uint16_t code, bool pressed)
{
	if ((code == INPUT_KEY_CAMERA_FOCUS) && pressed) {
		cam_focus();
	} else if ((code == INPUT_KEY_CAMERA) && pressed) {
		camera_shoot();
	}
}

static void shoot_cb(lv_event_t *e)
{
	camera_shoot();
}

/* Tapping the preview focuses */
static void preview_cb(lv_event_t *e)
{
	cam_focus();
}

static void flash_btn_update(void)
{
	lv_label_set_text(lv_obj_get_child(cam_flash_btn, 0),
			  cam_flash_on ? LV_SYMBOL_CHARGE " On" : LV_SYMBOL_CHARGE " Off");
	lv_obj_set_style_bg_color(cam_flash_btn, cam_flash_on ? lv_color_hex(0xF39C12) : COL_TILE,
				  0);
}

static void flash_btn_cb(lv_event_t *e)
{
	cam_flash_on = !cam_flash_on;
	flash_btn_update();
}

static void review_close_cb(lv_event_t *e)
{
	lv_obj_delete(cam_review);
	cam_review = NULL;
}

/* Full-screen view of the newest photo; tap to close */
static void thumb_cb(lv_event_t *e)
{
	lv_obj_t *img;

	if ((cam_shown_photo < 0) || (cam_review != NULL)) {
		return;
	}
	cam_review = lv_obj_create(lv_layer_top());
	lv_obj_set_size(cam_review, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(cam_review, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cam_review, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cam_review, 0, 0);
	lv_obj_set_style_radius(cam_review, 0, 0);
	lv_obj_set_scrollable(cam_review, false);
	lv_obj_add_event_cb(cam_review, review_close_cb, LV_EVENT_CLICKED, NULL);
	cam_dsc_init(&cam_photo_dsc, cam_photo(cam_shown_photo), CAM_PHOTO_W, CAM_PHOTO_H);
	img = lv_image_create(cam_review);
	lv_image_set_src(img, &cam_photo_dsc);
	lv_obj_center(img);
	label(cam_review, &lv_font_montserrat_14, COL_DIM, "Tap to close");
}

static void camera_leave(void)
{
	cam_stop();
	cam_want = false;
	if (cam_timer != NULL) {
		lv_timer_delete(cam_timer);
		cam_timer = NULL;
	}
	/* A rebuild may already have deleted the review with the top layer */
	if ((cam_review != NULL) && lv_obj_is_valid(cam_review)) {
		lv_obj_delete(cam_review);
	}
	cam_review = NULL;
	cam_preview_in_use(NULL);
}

static void build_camera(void)
{
	lv_obj_t *view, *b, *col;

	view = panel(content, CAM_PREVIEW_W + 8, CAM_PREVIEW_H + 8);
	lv_obj_set_style_pad_all(view, 4, 0);
	lv_obj_set_style_bg_color(view, lv_color_black(), 0);
	lv_obj_align(view, LV_ALIGN_LEFT_MID, 10, 0);
	lv_obj_set_clickable(view, true);
	lv_obj_add_event_cb(view, preview_cb, LV_EVENT_CLICKED, NULL);

	cam_img = lv_image_create(view);
	lv_obj_center(cam_img);
	lv_obj_set_hidden(cam_img, true);
	lv_obj_set_clickable(cam_img, false);

	cam_box = lv_obj_create(view);
	lv_obj_set_size(cam_box, CAM_PREVIEW_W / 3, CAM_PREVIEW_H / 3);
	lv_obj_center(cam_box);
	lv_obj_set_style_bg_opa(cam_box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cam_box, 3, 0);
	lv_obj_set_style_radius(cam_box, 6, 0);
	lv_obj_set_clickable(cam_box, false);

	cam_label = label(view, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_set_style_bg_color(cam_label, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cam_label, LV_OPA_50, 0);
	lv_obj_set_style_pad_hor(cam_label, 8, 0);
	lv_obj_align(cam_label, LV_ALIGN_BOTTOM_MID, 0, -6);

	cam_info = label(view, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_style_bg_color(cam_info, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cam_info, LV_OPA_50, 0);
	lv_obj_set_style_pad_all(cam_info, 4, 0);
	lv_obj_align(cam_info, LV_ALIGN_TOP_LEFT, 6, 6);

	/* Controls column */
	col = lv_obj_create(content);
	lv_obj_set_size(col, SCR_W - CAM_PREVIEW_W - 40, CAM_PREVIEW_H + 8);
	lv_obj_align(col, LV_ALIGN_RIGHT_MID, -10, 0);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(col, 0, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_scrollable(col, false);

	cam_flash_btn = button(col, "", COL_TILE, flash_btn_cb, NULL);
	lv_obj_set_size(cam_flash_btn, 130, 50);
	lv_obj_align(cam_flash_btn, LV_ALIGN_TOP_MID, 0, 0);
	flash_btn_update();

	b = button(col, LV_SYMBOL_IMAGE, COL_ACCENT, shoot_cb, NULL);
	lv_obj_set_size(b, 110, 110);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(b, LV_ALIGN_CENTER, 0, -10);

	cam_thumb = lv_image_create(col);
	lv_image_set_scale(cam_thumb, 256 / 5);
	lv_obj_set_size(cam_thumb, CAM_PHOTO_W / 5, CAM_PHOTO_H / 5);
	lv_image_set_inner_align(cam_thumb, LV_IMAGE_ALIGN_CENTER);
	lv_obj_align(cam_thumb, LV_ALIGN_BOTTOM_LEFT, 6, 0);
	lv_obj_set_clickable(cam_thumb, true);
	lv_obj_add_event_cb(cam_thumb, thumb_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(cam_thumb, true);
	cam_shown_photo = -1;

	cam_count = label(col, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_align(cam_count, LV_ALIGN_BOTTOM_RIGHT, -6, -30);

	cam_seq = 0;
	cam_timer = lv_timer_create(camera_frame_cb, 40, NULL);
	app_key = camera_key;
	app_tick = camera_tick;
	camera_tick();
}

/* ---------- sensors ---------- */

static lv_obj_t *lvl_bubble;
static lv_obj_t *sens_text;
static lv_obj_t *light_bar;
static lv_obj_t *tap_label;
static int64_t tap_flash_until;

static void sensors_tick(void)
{
	float ax = st.s.ax / 100.0f, ay = st.s.ay / 100.0f, az = st.s.az / 100.0f;
	float pitch = atan2f(ax, sqrtf(ay * ay + az * az)) * 57.3f;
	float roll = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.3f;
	int32_t bx = (int32_t)CLAMP(-st.s.ax * 85 / 981, -85, 85);
	int32_t by = (int32_t)CLAMP(st.s.ay * 85 / 981, -85, 85);

	/* The bubble moves in screen coordinates, which the rotation turns */
	switch (st.rotation) {
	case LV_DISPLAY_ROTATION_90: {
		int32_t t = bx;

		bx = by;
		by = -t;
		break;
	}
	case LV_DISPLAY_ROTATION_180:
		bx = -bx;
		by = -by;
		break;
	case LV_DISPLAY_ROTATION_270: {
		int32_t t = bx;

		bx = -by;
		by = t;
		break;
	}
	default:
		break;
	}
	lv_obj_align(lvl_bubble, LV_ALIGN_CENTER, bx, by);
	lv_label_set_text_fmt(
		sens_text,
		"Acceleration  x %d.%02d  y %d.%02d  z %d.%02d m/s2\n"
		"Tilt  %d deg forward, %d deg sideways\n"
		"Proximity  %s\nKeyboard  %s\nLens cover  %s\nBack cover  %s\n"
		"Battery  %d.%d C",
		st.s.ax / 100, abs(st.s.ax % 100), st.s.ay / 100, abs(st.s.ay % 100), st.s.az / 100,
		abs(st.s.az % 100), (int)roll, (int)pitch, st.near ? "NEAR" : "far",
		st.slide_open ? "open" : "closed", st.lens_closed ? "closed" : "open",
		st.cover_closed ? "closed" : "OPEN", st.s.temp_dc / 10, abs(st.s.temp_dc % 10));
	lv_bar_set_value(light_bar, MIN(st.s.lux, 50), LV_ANIM_ON);
	lv_label_set_text_fmt(lv_obj_get_child(light_bar, 0), "Light %d lux", st.s.lux);
	lv_label_set_text_fmt(tap_label, "Taps: %d", st.taps);
	lv_obj_set_style_text_color(tap_label,
				    (k_uptime_get() < tap_flash_until) ? COL_GREEN : COL_TEXT, 0);
}

static void build_sensors(void)
{
	lv_obj_t *left = PORTRAIT ? panel(content, SCR_W - 24, 330)
				  : panel(content, 260, SCR_H - BAR_H - 24);
	lv_obj_t *right = PORTRAIT ? panel(content, SCR_W - 24, SCR_H - BAR_H - 366)
				   : panel(content, 500, SCR_H - BAR_H - 24);
	lv_obj_t *ring = lv_obj_create(left);
	lv_obj_t *t;

	if (PORTRAIT) {
		lv_obj_align(left, LV_ALIGN_TOP_MID, 0, 12);
		lv_obj_align(right, LV_ALIGN_BOTTOM_MID, 0, -12);
	} else {
		lv_obj_align(left, LV_ALIGN_LEFT_MID, 12, 0);
		lv_obj_align(right, LV_ALIGN_RIGHT_MID, -12, 0);
	}

	lv_obj_set_size(ring, 220, 220);
	lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 10);
	lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ring, lv_color_hex(0x0B3D2E), 0);
	lv_obj_set_style_border_color(ring, COL_GREEN, 0);
	lv_obj_set_style_border_width(ring, 3, 0);
	lv_obj_set_scrollable(ring, false);
	t = lv_obj_create(ring);
	lv_obj_set_size(t, 60, 60);
	lv_obj_center(t);
	lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_color(t, COL_DIM, 0);
	lv_obj_set_style_border_width(t, 2, 0);
	lvl_bubble = lv_obj_create(ring);
	lv_obj_set_size(lvl_bubble, 40, 40);
	lv_obj_set_style_radius(lvl_bubble, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(lvl_bubble, lv_color_hex(0xB8F5A0), 0);
	lv_obj_set_style_border_width(lvl_bubble, 0, 0);
	tap_label = label(left, &lv_font_montserrat_28, COL_TEXT, "");
	lv_obj_align(tap_label, LV_ALIGN_BOTTOM_MID, 0, -30);
	t = label(left, &lv_font_montserrat_14, COL_DIM, "Tap the phone");
	lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, 0);

	sens_text = label(right, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_align(sens_text, LV_ALIGN_TOP_LEFT, 0, 0);
	light_bar = lv_bar_create(right);
	lv_bar_set_range(light_bar, 0, 50);
	lv_obj_set_size(light_bar, lv_pct(100), 40);
	lv_obj_align(light_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(light_bar, lv_color_hex(0xF1C40F), LV_PART_INDICATOR);
	t = label(light_bar, &lv_font_montserrat_20, lv_color_black(), "");
	lv_obj_center(t);

	app_tick = sensors_tick;
	sensors_tick();
}

/* ---------- settings ---------- */

static lv_obj_t *batt_text;
static lv_obj_t *bright_slider;

static void bright_cb(lv_event_t *e)
{
	set_brightness((uint8_t)lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void auto_cb(lv_event_t *e)
{
	st.auto_brightness = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static void rotate_cb(lv_event_t *e)
{
	st.auto_rotate = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static void kbd_cb(lv_event_t *e)
{
	st.kbd_level = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
}

static void led_test_cb(lv_event_t *e)
{
	uint32_t rgb = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

	st.led_test_rgb[0] = (rgb >> 16) & 0xFF;
	st.led_test_rgb[1] = (rgb >> 8) & 0xFF;
	st.led_test_rgb[2] = rgb & 0xFF;
	st.led_test_until = k_uptime_get() + 1500;
}

static const char *charger_text(int status)
{
	switch (status) {
	case CHARGER_STATUS_CHARGING:
		return "charging";
	case CHARGER_STATUS_FULL:
		return "full";
	case CHARGER_STATUS_DISCHARGING:
		return "on battery";
	case CHARGER_STATUS_NOT_CHARGING:
		return "not charging";
	default:
		return "unknown";
	}
}

static void settings_tick(void)
{
	lv_label_set_text_fmt(batt_text,
			      "Charge %d%%   %d.%02d V   %d mA\n"
			      "Temperature %d.%d C   Charger %s   USB %s\n\n"
			      "Zephyr %s on Nokia N900\n"
			      "TI OMAP3430, ARM Cortex-A8, 256 MB, up %lld s",
			      st.s.soc, st.s.mv / 1000, (st.s.mv % 1000) / 10, st.s.ma,
			      st.s.temp_dc / 10, abs(st.s.temp_dc % 10), charger_text(st.s.charger),
			      st.s.vbus ? "connected" : "unplugged", KERNEL_VERSION_STRING,
			      k_uptime_get() / 1000);
	if (st.auto_brightness) {
		lv_slider_set_value(bright_slider, st.brightness, LV_ANIM_ON);
	}
}

static lv_obj_t *setting_row(lv_obj_t *parent, const char *name, int32_t y)
{
	lv_obj_t *l = label(parent, &lv_font_montserrat_20, COL_TEXT, name);

	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
	return l;
}

static void volume_cb(lv_event_t *e)
{
	st.volume = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
	sound_set_volume(st.volume);
}

/* A sample at the new volume once the slider is let go */
static void volume_done_cb(lv_event_t *e)
{
	sound_play(SOUND_MESSAGE);
}

static void key_sounds_cb(lv_event_t *e)
{
	st.key_sounds = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static void build_settings(void)
{
	lv_obj_t *p = panel(content, SCR_W - 24, SCR_H - BAR_H - 24);
	lv_obj_t *w;

	lv_obj_center(p);

	/* Controls beside their label in landscape, below it in portrait */
	int32_t cx = PORTRAIT ? 0 : 240;
	int32_t cy = PORTRAIT ? 34 : 4;
	int32_t row = PORTRAIT ? 90 : 60;

	setting_row(p, "Screen brightness", 8);
	bright_slider = lv_slider_create(p);
	lv_slider_set_range(bright_slider, 10, 255);
	lv_slider_set_value(bright_slider, st.brightness, LV_ANIM_OFF);
	lv_obj_set_width(bright_slider, 280);
	lv_obj_align(bright_slider, LV_ALIGN_TOP_LEFT, cx, 8 + cy + 4);
	lv_obj_add_event_cb(bright_slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

	w = label(p, &lv_font_montserrat_20, COL_TEXT, "Auto");
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 300, 8 + cy);
	w = lv_switch_create(p);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 360, 8 + cy - 4);
	if (st.auto_brightness) {
		lv_obj_add_state(w, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(w, auto_cb, LV_EVENT_VALUE_CHANGED, NULL);

	setting_row(p, "Keyboard light", 8 + row);
	w = lv_slider_create(p);
	lv_slider_set_range(w, 0, 100);
	lv_slider_set_value(w, st.kbd_level, LV_ANIM_OFF);
	lv_obj_set_width(w, 280);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx, 8 + row + cy + 4);
	lv_obj_add_event_cb(w, kbd_cb, LV_EVENT_VALUE_CHANGED, NULL);

	w = label(p, &lv_font_montserrat_20, COL_TEXT, "Rotate");
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 300, 8 + row + cy);
	w = lv_switch_create(p);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 380, 8 + row + cy - 4);
	if (st.auto_rotate) {
		lv_obj_add_state(w, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(w, rotate_cb, LV_EVENT_VALUE_CHANGED, NULL);

	setting_row(p, "Status LED", 8 + 2 * row);
	w = button(p, "Red", COL_RED, led_test_cb, (void *)(uintptr_t)0x640000);
	lv_obj_set_size(w, 100, 46);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx, 8 + 2 * row + cy - 6);
	w = button(p, "Green", COL_GREEN, led_test_cb, (void *)(uintptr_t)0x006400);
	lv_obj_set_size(w, 100, 46);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 110, 8 + 2 * row + cy - 6);
	w = button(p, "Blue", COL_ACCENT, led_test_cb, (void *)(uintptr_t)0x000064);
	lv_obj_set_size(w, 100, 46);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 220, 8 + 2 * row + cy - 6);

	setting_row(p, "Volume", 8 + 3 * row);
	w = lv_slider_create(p);
	lv_slider_set_range(w, 0, 100);
	lv_slider_set_value(w, st.volume, LV_ANIM_OFF);
	lv_obj_set_width(w, 280);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx, 8 + 3 * row + cy + 4);
	lv_obj_add_event_cb(w, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(w, volume_done_cb, LV_EVENT_RELEASED, NULL);

	w = label(p, &lv_font_montserrat_20, COL_TEXT, "Keys");
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 300, 8 + 3 * row + cy);
	w = lv_switch_create(p);
	lv_obj_align(w, LV_ALIGN_TOP_LEFT, cx + 380, 8 + 3 * row + cy - 4);
	if (st.key_sounds) {
		lv_obj_add_state(w, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(w, key_sounds_cb, LV_EVENT_VALUE_CHANGED, NULL);

	batt_text = label(p, &lv_font_montserrat_20, COL_DIM, "");
	lv_label_set_long_mode(batt_text, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(batt_text, lv_pct(100));
	lv_obj_align(batt_text, LV_ALIGN_BOTTOM_LEFT, 0, 0);

	app_tick = settings_tick;
	settings_tick();
}

/* ---------- torch ---------- */

static lv_obj_t *torch_btn;

static void torch_update(void)
{
	lv_obj_set_style_bg_color(torch_btn, st.torch ? lv_color_hex(0xFFE066) : COL_TILE, 0);
	lv_label_set_text(lv_obj_get_child(torch_btn, 0),
			  st.torch ? LV_SYMBOL_EYE_OPEN "\n\nON" : LV_SYMBOL_EYE_CLOSE "\n\nOFF");
}

static void torch_cb(lv_event_t *e)
{
	st.torch = !st.torch;
	hw_torch(st.torch);
	torch_update();
}

static void build_torch(void)
{
	lv_obj_t *l;

	torch_btn = button(content, "", COL_TILE, torch_cb, NULL);
	lv_obj_set_size(torch_btn, 300, 300);
	lv_obj_set_style_radius(torch_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_center(torch_btn);
	lv_obj_set_style_text_font(torch_btn, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_align(lv_obj_get_child(torch_btn, 0), LV_TEXT_ALIGN_CENTER, 0);
	l = label(content, &lv_font_montserrat_14, COL_DIM,
		  "Status LED and keyboard backlight at full power");
	lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -8);
	torch_update();
}

/* ---------- terminal ---------- */

/*
 * The shell on a character grid of TERM_COLS x TERM_ROWS, with the scrollback
 * above it. Each line is a row of labels, one for each run of a colour, so
 * that no character needs escaping; the cursor is a block over the grid.
 */
#define TERM_CELL_W 8
#define TERM_CELL_H 18
#define TERM_PAD    4

static const uint32_t term_palette[] = {
	0xD8DEE4, /* default */
	0x7F8C8D, /* black, which would not show */
	0xE74C3C, 0x2ECC71, 0xF1C40F, 0x3DA5FF, 0xC678DD, 0x56B6C2, 0xFFFFFF,
};

static lv_obj_t *term_box;
static lv_obj_t *term_cursor;
static lv_obj_t *term_ctrl_button;
/* Line of the first row, and the number of rows */
static uint32_t term_first;
static uint32_t term_rows;
static bool term_ctrl_held;
static bool term_ctrl_once;

static lv_obj_t *term_row_create(void)
{
	lv_obj_t *r = lv_obj_create(term_box);

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, TERM_COLS * TERM_CELL_W, TERM_CELL_H);
	lv_obj_set_clickable(r, false);
	lv_obj_set_scrollable(r, false);
	return r;
}

static void term_row_render(lv_obj_t *r, const struct term_cell *cells)
{
	char text[TERM_COLS + 1];
	uint16_t len = TERM_COLS;
	uint16_t start = 0U;

	lv_obj_clean(r);

	while ((len > 0U) && (cells[len - 1U].c == ' ')) {
		len--;
	}

	while (start < len) {
		uint8_t color = cells[start].color;
		uint16_t n = 0U;
		lv_obj_t *l;

		while (((start + n) < len) && (cells[start + n].color == color)) {
			text[n] = cells[start + n].c;
			n++;
		}
		text[n] = '\0';

		l = label(r, &lv_font_adwaita_mono_14,
			  lv_color_hex(term_palette[MIN(color, ARRAY_SIZE(term_palette) - 1U)]),
			  text);
		lv_obj_set_pos(l, start * TERM_CELL_W, 0);
		start += n;
	}
}

static void term_scroll_bottom(void)
{
	lv_obj_update_layout(term_box);
	lv_obj_scroll_to_y(term_box,
			   lv_obj_get_scroll_y(term_box) + lv_obj_get_scroll_bottom(term_box),
			   LV_ANIM_OFF);
}

/* Brings the rows up to date with the grid */
static void term_refresh(void)
{
	struct term_view v;
	bool follow;

	(void)term_take_changed();
	if ((st.app != APP_TERMINAL) || (term_box == NULL)) {
		return;
	}

	/* Output scrolls the view along, unless it was scrolled back */
	follow = lv_obj_get_scroll_bottom(term_box) <= TERM_CELL_H;

	term_lock(&v);

	/* The first child is the cursor */
	while ((term_rows > 0U) && (term_first < v.first)) {
		lv_obj_delete(lv_obj_get_child(term_box, 1));
		term_first++;
		term_rows--;
	}
	if (term_rows == 0U) {
		term_first = v.first;
	}

	for (uint32_t line = term_first; line < v.end; line++) {
		uint32_t i = line - term_first;
		bool dirty = term_take_dirty(line);

		if (i >= term_rows) {
			term_row_render(term_row_create(), term_line(line));
			term_rows++;
		} else if (dirty) {
			term_row_render(lv_obj_get_child(term_box, (int32_t)(1U + i)),
					term_line(line));
		}
	}

	lv_obj_set_pos(term_cursor, v.cursor_col * TERM_CELL_W,
		       (int32_t)(v.cursor_line - term_first) * TERM_CELL_H);

	term_unlock();

	if (follow) {
		term_scroll_bottom();
	}
}

static void term_tick(void)
{
	if (term_take_changed()) {
		term_refresh();
	}
}

static void term_ctrl_show(void)
{
	lv_obj_set_style_bg_color(term_ctrl_button,
				  (term_ctrl_once || term_ctrl_held) ? COL_ACCENT : COL_TILE, 0);
}

static void term_send(const char *data, size_t len)
{
	term_shell_input(data, len);
	term_scroll_bottom();
}

/* Typed characters; Ctrl turns letters into control characters */
static void term_char(char c)
{
	char out = c;

	if (c == '\n') {
		out = '\r';
	} else if ((term_ctrl_held || term_ctrl_once) &&
		   (((c >= '@') && (c <= '_')) || ((c >= 'a') && (c <= 'z')))) {
		out = (char)(c & 0x1F);
	}

	if (term_ctrl_once) {
		term_ctrl_once = false;
		term_ctrl_show();
	}

	term_send(&out, 1);
	if (st.key_sounds) {
		sound_play(SOUND_CLICK);
	}
}

/* The arrow keys go to the shell as VT100 sequences: history and cursor */
static void term_key(uint16_t code, bool pressed)
{
	static const struct {
		uint16_t code;
		const char *seq;
	} arrows[] = {
		{INPUT_KEY_UP, "\x1b[A"},
		{INPUT_KEY_DOWN, "\x1b[B"},
		{INPUT_KEY_RIGHT, "\x1b[C"},
		{INPUT_KEY_LEFT, "\x1b[D"},
	};

	if (code == INPUT_KEY_LEFTCTRL) {
		term_ctrl_held = pressed;
		term_ctrl_show();
		return;
	}

	if (!pressed) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(arrows); i++) {
		if (arrows[i].code == code) {
			term_send(arrows[i].seq, strlen(arrows[i].seq));
			return;
		}
	}
}

/* The touch keys send what the keyboard has no key for */
static void term_key_cb(lv_event_t *e)
{
	const char *seq = lv_event_get_user_data(e);

	term_send(seq, strlen(seq));
}

static void term_ctrl_cb(lv_event_t *e)
{
	ARG_UNUSED(e);

	term_ctrl_once = !term_ctrl_once;
	term_ctrl_show();
}

static void build_terminal(void)
{
	static const struct {
		const char *text;
		const char *seq;
	} keys[] = {
		{"Tab", "\t"},
		{"^C", "\x03"},
		{"Clear", "\x0c"},
	};
	int32_t box_w = TERM_COLS * TERM_CELL_W + 2 * TERM_PAD;
	int32_t box_h = TERM_ROWS * TERM_CELL_H + 2 * TERM_PAD;
	int32_t box_y = (SCR_H - BAR_H - box_h) / 2;
	int32_t key_x = 8 + box_w + 8;
	int32_t key_w = SCR_W - key_x - 8;
	int32_t key_h = (box_h - 3 * 8) / 4;
	lv_obj_t *b;

	term_box = lv_obj_create(content);
	lv_obj_set_size(term_box, box_w, box_h);
	lv_obj_set_pos(term_box, 8, box_y);
	lv_obj_set_style_bg_color(term_box, lv_color_hex(0x0B0F14), 0);
	lv_obj_set_style_border_width(term_box, 0, 0);
	lv_obj_set_style_radius(term_box, 6, 0);
	lv_obj_set_style_pad_all(term_box, TERM_PAD, 0);
	lv_obj_set_style_pad_row(term_box, 0, 0);
	lv_obj_set_flex_flow(term_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(term_box, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(term_box, LV_SCROLLBAR_MODE_ACTIVE);

	term_cursor = lv_obj_create(term_box);
	lv_obj_remove_style_all(term_cursor);
	lv_obj_set_ignore_layout(term_cursor, true);
	lv_obj_set_clickable(term_cursor, false);
	lv_obj_set_scrollable(term_cursor, false);
	lv_obj_set_size(term_cursor, TERM_CELL_W, TERM_CELL_H);
	lv_obj_set_style_bg_color(term_cursor, COL_ACCENT, 0);
	lv_obj_set_style_bg_opa(term_cursor, LV_OPA_50, 0);

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		b = button(content, keys[i].text, COL_TILE, term_key_cb, (void *)keys[i].seq);
		lv_obj_set_size(b, key_w, key_h);
		lv_obj_set_pos(b, key_x, box_y + (int32_t)i * (key_h + 8));
	}
	term_ctrl_button = button(content, "Ctrl", COL_TILE, term_ctrl_cb, NULL);
	lv_obj_set_size(term_ctrl_button, key_w, key_h);
	lv_obj_set_pos(term_ctrl_button, key_x, box_y + 3 * (key_h + 8));

	term_first = 0U;
	term_rows = 0U;
	term_ctrl_once = false;
	term_ctrl_show();

	app_char = term_char;
	app_key = term_key;
	app_tick = term_tick;
	term_refresh();
}

/* ---------- app switching ---------- */

static void open_app(enum app_id app)
{
	static const char *const titles[] = {"Home",	"Phone",    "Messages", "Camera",
					     "Sensors", "Settings", "Torch",	"Terminal"};

	if ((st.app == APP_CAMERA) && (cam_timer != NULL)) {
		camera_leave();
	}
	lv_obj_clean(content);
	app_tick = NULL;
	app_char = NULL;
	app_key = NULL;
	chat = NULL;
	st.app = app;
	lv_label_set_text(bar_title, titles[app]);
	lv_obj_set_hidden(bar_home, app == APP_HOME);
	lv_obj_set_hidden(bar_kite, app != APP_HOME);

	switch (app) {
	case APP_HOME:
		build_home();
		break;
	case APP_PHONE:
		build_phone();
		break;
	case APP_MESSAGES:
		build_messages();
		break;
	case APP_CAMERA:
		build_camera();
		break;
	case APP_SENSORS:
		build_sensors();
		break;
	case APP_SETTINGS:
		build_settings();
		break;
	case APP_TORCH:
		build_torch();
		break;
	case APP_TERMINAL:
		build_terminal();
		break;
	}
}

/* ---------- periodic work ---------- */

static void tick_cb(lv_timer_t *t)
{
	static int64_t last_slow;
	int64_t now = k_uptime_get();

	phone_sensors_get(&st.s);
	leds_update();
	if (st.s.accel_ok) {
		rotation_update();
	}

	if (st.call != CALL_NONE) {
		call_tick();
		/* Proximity blanks the screen next to the ear */
		if (st.call == CALL_ACTIVE) {
			screen_set(!st.near);
		}
	}

	if (app_tick != NULL) {
		app_tick();
	}

	if ((now - last_slow) >= 1000) {
		char buf[16];

		last_slow = now;
		bar_update();
		clock_text(buf, sizeof(buf), false);
		lv_label_set_text(lock_clock, buf);
		if (st.auto_brightness && st.s.light_ok) {
			set_brightness((uint8_t)CLAMP(40 + st.s.lux * 15, 40, 255));
		}
	}
}

/* ---------- input ---------- */

static const struct {
	uint16_t code;
	char c;
	char fn;
} keymap[] = {
	{INPUT_KEY_Q, 'q', '1'},     {INPUT_KEY_W, 'w', '2'},	   {INPUT_KEY_E, 'e', '3'},
	{INPUT_KEY_R, 'r', '4'},     {INPUT_KEY_T, 't', '5'},	   {INPUT_KEY_Y, 'y', '6'},
	{INPUT_KEY_U, 'u', '7'},     {INPUT_KEY_I, 'i', '8'},	   {INPUT_KEY_O, 'o', '9'},
	{INPUT_KEY_P, 'p', '0'},     {INPUT_KEY_A, 'a', '*'},	   {INPUT_KEY_S, 's', '+'},
	{INPUT_KEY_D, 'd', '#'},     {INPUT_KEY_F, 'f', '-'},	   {INPUT_KEY_G, 'g', '_'},
	{INPUT_KEY_H, 'h', '('},     {INPUT_KEY_J, 'j', ')'},	   {INPUT_KEY_K, 'k', '&'},
	{INPUT_KEY_L, 'l', '!'},     {INPUT_KEY_Z, 'z', 0},	   {INPUT_KEY_X, 'x', 0},
	{INPUT_KEY_C, 'c', 0},	     {INPUT_KEY_V, 'v', '/'},	   {INPUT_KEY_B, 'b', '\\'},
	{INPUT_KEY_N, 'n', '"'},     {INPUT_KEY_M, 'm', '\''},	   {INPUT_KEY_SPACE, ' ', ' '},
	{INPUT_KEY_DOT, '.', ':'},   {INPUT_KEY_COMMA, ',', ';'},  {INPUT_KEY_ENTER, '\n', '\n'},
	{INPUT_KEY_BACKSPACE, '\b', '\b'},
};

static void key_event(uint16_t code, bool pressed)
{
	switch (code) {
	case INPUT_KEY_LEFTSHIFT:
		st.shift = pressed;
		return;
	case INPUT_KEY_RIGHTALT:
		st.fn = pressed;
		return;
	case INPUT_KEY_COFFEE:
		lock_set(pressed);
		return;
	case INPUT_KEY_POWER:
		/* Short press toggles the screen; the board reboots on a long hold */
		if (pressed) {
			st.power_down_at = k_uptime_get();
		} else if ((k_uptime_get() - st.power_down_at) < 1000) {
			if (st.screen_on) {
				screen_set(false);
			} else if (st.locked) {
				lv_obj_set_hidden(lock_layer, false);
				screen_set(true);
			} else {
				screen_set(true);
			}
		}
		return;
	default:
		break;
	}

	if (st.locked) {
		return;
	}

	if (!st.screen_on && pressed) {
		screen_set(true);
		return;
	}

	if ((code == INPUT_KEY_CAMERA) && pressed && (st.app != APP_CAMERA)) {
		open_app(APP_CAMERA);
		return;
	}

	if (app_key != NULL) {
		app_key(code, pressed);
	}

	if (!pressed || (app_char == NULL)) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(keymap); i++) {
		if (keymap[i].code == code) {
			char c = st.fn ? keymap[i].fn : keymap[i].c;

			if (c == 0) {
				return;
			}
			if (st.shift && (c >= 'a') && (c <= 'z')) {
				c = c - 'a' + 'A';
			}
			app_char(c);
			return;
		}
	}
}

static void switch_event(uint16_t code, bool on)
{
	switch (code) {
	case INPUT_SW_KEYPAD_SLIDE:
		if (on != st.slide_open) {
			st.slide_open = on;
			if (k_uptime_get() > 3000) {
				show_toast(on ? LV_SYMBOL_KEYBOARD "  Keyboard open"
					      : LV_SYMBOL_KEYBOARD "  Keyboard closed");
			}
			if (on && !st.locked) {
				screen_set(true);
			}
		}
		break;
	case INPUT_SW_FRONT_PROXIMITY:
		st.near = on;
		break;
	case INPUT_SW_CAMERA_LENS_COVER:
		st.lens_closed = on;
		if (!on && !st.locked && (k_uptime_get() > 3000)) {
			open_app(APP_CAMERA);
			screen_set(true);
		}
		break;
	case INPUT_SW_MACHINE_COVER:
		st.cover_closed = on;
		if (!on) {
			show_toast(LV_SYMBOL_WARNING "  Back cover removed");
		}
		break;
	default:
		break;
	}
}

void ui_event(const struct phone_event *e)
{
	switch (e->type) {
	case PEV_KEY:
		key_event(e->code, e->value != 0);
		break;
	case PEV_SWITCH:
		switch_event(e->code, e->value != 0);
		break;
	case PEV_TAP:
		st.taps++;
		tap_flash_until = k_uptime_get() + 400;
		if (!st.screen_on && !st.locked && (st.call != CALL_ACTIVE)) {
			screen_set(true);
		}
		break;
	case PEV_USB:
		if (e->value) {
			sound_play(SOUND_CHARGER);
		}
		show_toast(e->value ? LV_SYMBOL_USB "  USB connected, charging"
				    : LV_SYMBOL_USB "  USB disconnected");
		break;
	case PEV_TERM:
		term_shell_ack();
		if (st.app == APP_TERMINAL) {
			term_refresh();
		}
		break;
	default:
		break;
	}
}

/* ---------- init ---------- */

static lv_obj_t *layer_obj(lv_color_t color)
{
	lv_obj_t *o = lv_obj_create(lv_layer_top());

	lv_obj_set_size(o, SCR_W, SCR_H);
	lv_obj_set_pos(o, 0, 0);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_radius(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_hidden(o, true);
	return o;
}

/* Status bar, content area and the overlays, for the current resolution */
static void build_frame(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_t *o;

	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_text_color(scr, COL_TEXT, 0);

	lv_obj_set_size(bar, SCR_W, BAR_H);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, COL_BAR, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_set_scrollable(bar, false);

	bar_home = button(bar, LV_SYMBOL_HOME, COL_TILE, home_cb, NULL);
	lv_obj_set_size(bar_home, 70, 36);
	lv_obj_align(bar_home, LV_ALIGN_LEFT_MID, 6, 0);
	/* The kite stands in for the home button on the home screen */
	bar_kite = lv_image_create(bar);
	lv_image_set_src(bar_kite, &kite_small);
	lv_obj_align(bar_kite, LV_ALIGN_LEFT_MID, 18, 0);
	bar_title = label(bar, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_align(bar_title, LV_ALIGN_LEFT_MID, 90, 0);
	bar_clock = label(bar, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_align(bar_clock, LV_ALIGN_CENTER, 0, 0);
	bar_status = label(bar, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_align(bar_status, LV_ALIGN_RIGHT_MID, -10, 0);

	content = lv_obj_create(scr);
	lv_obj_set_size(content, SCR_W, SCR_H - BAR_H);
	lv_obj_set_pos(content, 0, BAR_H);
	lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_radius(content, 0, 0);
	lv_obj_set_style_pad_all(content, 0, 0);
	lv_obj_set_scrollable(content, false);

	/* Toast below the status bar */
	toast = lv_obj_create(lv_layer_top());
	lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, BAR_H + 8);
	lv_obj_set_style_bg_color(toast, lv_color_hex(0x2D3436), 0);
	lv_obj_set_style_border_color(toast, COL_ACCENT, 0);
	lv_obj_set_style_border_width(toast, 2, 0);
	lv_obj_set_style_radius(toast, 20, 0);
	lv_obj_set_style_pad_hor(toast, 20, 0);
	lv_obj_set_style_pad_ver(toast, 10, 0);
	label(toast, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_set_hidden(toast, true);

	/* Call screen */
	call_layer = layer_obj(lv_color_hex(0x0A2A1A));
	o = label(call_layer, &lv_font_montserrat_48, COL_TEXT, "");
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 60);
	o = label(call_layer, &lv_font_montserrat_28, COL_DIM, "");
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 130);
	call_status = label(call_layer, &lv_font_montserrat_28, COL_GREEN, "");
	lv_obj_align(call_status, LV_ALIGN_TOP_MID, 0, 190);
	call_buttons = lv_obj_create(call_layer);
	lv_obj_set_size(call_buttons, SCR_W - 100, 120);
	lv_obj_align(call_buttons, LV_ALIGN_BOTTOM_MID, 0, -30);
	lv_obj_set_style_bg_opa(call_buttons, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(call_buttons, 0, 0);
	lv_obj_set_flex_flow(call_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(call_buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	/* Lock screen */
	lock_layer = layer_obj(lv_color_hex(0x000000));
	o = lv_image_create(lock_layer);
	lv_image_set_src(o, &kite_medium);
	lv_obj_set_style_image_opa(o, LV_OPA_80, 0);
	lv_obj_align(o, LV_ALIGN_CENTER, 0, -130);
	lock_clock = label(lock_layer, &lv_font_montserrat_48, COL_TEXT, "");
	lv_obj_align(lock_clock, LV_ALIGN_CENTER, 0, -30);
	o = label(lock_layer, &lv_font_montserrat_20, COL_DIM,
		  LV_SYMBOL_EYE_CLOSE "  Locked - slide the lock switch to unlock");
	lv_obj_align(o, LV_ALIGN_CENTER, 0, 40);

	/* Camera flash */
	flash_layer = layer_obj(lv_color_white());
}

static void rebuild(void)
{
	lv_obj_clean(lv_screen_active());
	lv_obj_clean(lv_layer_top());
	build_frame();

	if (st.locked) {
		lv_obj_set_hidden(lock_layer, false);
	}
	if (st.call != CALL_NONE) {
		lv_label_set_text(lv_obj_get_child(call_layer, 0), st.call_name);
		lv_label_set_text(lv_obj_get_child(call_layer, 1), st.call_number);
		call_buttons_set(st.call == CALL_RINGING);
		lv_obj_set_hidden(call_layer, false);
	}
	open_app(st.app);
	bar_update();
}

/*
 * Orientation from gravity: along -Y with the phone upright in landscape,
 * along +X or -X in portrait. A direction counts once it holds for half a
 * second; lying flat keeps the current one.
 */
#define ROT_G_MIN 650 /* centi m/s^2 */
#define ROT_TICKS 5

static void rotation_update(void)
{
	lv_display_rotation_t want = st.rotation;

	/* The camera and the terminal's character grid stay in landscape */
	if (st.slide_open || !st.auto_rotate || (st.app == APP_CAMERA) ||
	    (st.app == APP_TERMINAL)) {
		want = LV_DISPLAY_ROTATION_0;
	} else if (st.s.ay < -ROT_G_MIN) {
		want = LV_DISPLAY_ROTATION_0;
	} else if (st.s.ay > ROT_G_MIN) {
		want = LV_DISPLAY_ROTATION_180;
	} else if (st.s.ax > ROT_G_MIN) {
		want = LV_DISPLAY_ROTATION_270;
	} else if (st.s.ax < -ROT_G_MIN) {
		want = LV_DISPLAY_ROTATION_90;
	}

	if (want == st.rotation) {
		st.rot_ticks = 0;
		return;
	}
	if (want != st.rot_candidate) {
		st.rot_candidate = want;
		st.rot_ticks = 0;
	}
	if (++st.rot_ticks < ROT_TICKS) {
		return;
	}

	st.rotation = want;
	st.rot_ticks = 0;
	lv_display_set_rotation(NULL, want);
	rebuild();
}

void ui_init(void)
{
	int h = 0, m = 0, s = 0;

	sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
	build_secs = h * 3600 + m * 60 + s;

	toast_timer = lv_timer_create(toast_hide, 2500, NULL);
	lv_timer_pause(toast_timer);

	cam_init();
	sound_init();
	sound_set_volume(st.volume);
	build_frame();
	open_app(APP_HOME);
	show_splash();
	lv_timer_create(tick_cb, 100, NULL);
	lv_timer_handler();
	hw_screen(true, st.brightness);
}
