/* N900 phone demo: shared definitions */
#ifndef PHONE_H_
#define PHONE_H_

#include <stdbool.h>
#include <stdint.h>

enum phone_event_type {
	PEV_KEY,    /* code, value: key press/release */
	PEV_SWITCH, /* code, value: INPUT_SW_* */
	PEV_TAP,    /* accelerometer tap */
	PEV_USB,    /* value: VBUS present */
	PEV_TERM,   /* the terminal has new output */
};

struct phone_event {
	uint8_t type;
	uint16_t code;
	int32_t value;
};

/* Latest sensor readings, refreshed by the sensor threads */
struct phone_sensors {
	bool gauge_ok;
	int soc;           /* % */
	int mv;            /* battery voltage */
	int ma;            /* battery current, positive while charging */
	int temp_dc;       /* deci-degrees Celsius */
	int charger;       /* enum charger_status */
	bool vbus;
	bool light_ok;
	int lux;
	bool accel_ok;
	int ax, ay, az;    /* centi m/s^2 */
};

void phone_post(uint8_t type, uint16_t code, int32_t value);
void phone_sensors_get(struct phone_sensors *out);

/* Hardware control, LVGL thread only */
void hw_init(void);
void hw_screen(bool on, uint8_t brightness);
void hw_kbd_backlight(uint8_t pct);
void hw_status_led(uint8_t r, uint8_t g, uint8_t b);
void hw_torch(bool on);
void hw_vibrate(uint32_t ms);

/* UI, LVGL thread only */
void ui_init(void);
void ui_event(const struct phone_event *e);

#endif
