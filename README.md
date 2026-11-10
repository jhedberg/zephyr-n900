# Zephyr on the Nokia N900

A Zephyr module with everything needed to run Zephyr on the Nokia N900
(RX-51, TI OMAP3430, Arm Cortex-A8), kept outside the Zephyr tree:

- `soc/ti/omap3`: the OMAP3 SoC series (entry stub, clocks, MMU regions,
  display console, boot stage markers)
- `boards/nokia/n900`: the board
- `dts/`: the OMAP3430 devicetree and the bindings of all drivers below
- `drivers/`: interrupt controller, GPTIMER system timer, I2C, GPIO,
  pinctrl, display controller and panel, TWL4030 (MFD, keypad, power
  button, GPIO, vibrator), BQ24150A charger, BQ27200 fuel gauge, TSC2005
  touchscreen, LIS302DL accelerometer, LP5523 and ADP1653 LED drivers,
  MUSB USB device controller, hwinfo, McBSP I2S and TLV320AIC3x codec
- `scripts/runners/nolo.py`: a west runner loading images over USB into
  RAM through the NOLO bootloader with `0xFFFF`
- `apps/`: the phone demo, the camera preview, audio playback, and the
  bring-up diagnostics in `apps/bringup/`
- `tests/`: a build test of the drivers on `native_sim`
- `tools/`: the bare-metal smoke test and the host scripts used during
  bring-up

A few changes to existing Zephyr code have no out-of-tree equivalent and
live on the `topic/n900/core` branch of Zephyr, which `west.yml` points at:
Cortex-A8 CPU support, two Armv7 MMU fixes, the `-mfpu` mapping, McSPI and
TSL2561 driver changes, input switch events and LVGL software rotation.

## Getting started

```sh
west init -m https://github.com/nashif/zephyr-n900 --mr main n900-workspace
cd n900-workspace
west update
west build -b n900 zephyr-n900/apps/phone
west flash
```

To build against an existing Zephyr checkout instead, point Zephyr at the
module:

```sh
export ZEPHYR_EXTRA_MODULES=/path/to/zephyr-n900
west build -b n900 /path/to/zephyr-n900/apps/audioplay
```

The phone and camera apps generate the camera sensor register tables at
build time from a local Linux tree (`-DLINUX_DIR=...`, default `~/linux`);
those tables are GPL-2.0 and are not part of this repository.

`-DEXTRA_CONF_FILE=overlay-bt.conf` gives the phone demo a Bluetooth app:
switching it on and off, visibility, discovery, pairing and the paired
devices, and the device name, with an address derived from the OMAP die
ID. It needs the BCM2048 firmware script in
`CONFIG_BT_NOKIA_H4P_FIRMWARE` (the board documentation says where to get
it) and Host support for controllers without LE
(zephyrproject-rtos/zephyr#125120). Encrypted links also need
`CONFIG_BT_BR_ALLOW_UNKNOWN_ENC_KEY_SIZE=y`
(zephyrproject-rtos/zephyr#125213).

The phone demo's Terminal app runs the Zephyr shell on the screen, in
88 x 23 characters with the log in between, and stays in landscape. The
arrow keys browse the history and move the cursor, the Sym/Ctrl key makes
control characters (Ctrl+I is Tab), and touch keys give Tab, Ctrl-C, Clear
and a one-shot Ctrl. Its font is Adwaita Mono, converted by
`apps/phone/gen_font.py`; it is under the SIL Open Font License 1.1, whose
text is in the font's source file.

`west flash` needs `0xFFFF` and the phone in NOLO's USB mode;
`scripts/autoload.sh` waits for NOLO and loads an image whenever the phone
reboots.

## Tests

```sh
west twister -T apps -p n900 --exclude-tag needs-linux
west twister -T tests -p native_sim
ZEPHYR_BASE=/path/to/zephyr pytest scripts/tests
```
