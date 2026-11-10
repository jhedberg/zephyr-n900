.. zephyr:board:: n900

Overview
********

The Nokia N900 (RX-51) is a 2009 smartphone built around the TI OMAP3430 SoC: a single ARM
Cortex-A8 core, 256 MB of SDRAM and an 800x480 LCD.

Zephyr runs from SDRAM after the phone's bootloader, NOLO, has initialized the SoC, the memory
and the display. The image is loaded into RAM over USB; nothing is written to flash.

Hardware
********

Supported Features
==================

.. zephyr:board-supported-hw::

Console
=======

The console is drawn on the LCD: the SoC takes over the graphics layer that NOLO configured and
renders ``printk()`` and standard output with the character framebuffer fonts
(:kconfig:option:`CONFIG_SOC_OMAP3_DSS_CONSOLE`).

UART3 is available on test pads under the battery at 1.8 V and is described in the devicetree,
but it is not the default console.

Display
=======

NOLO configures the panel and the display controller. The display driver
(:kconfig:option:`CONFIG_TI_OMAP3_DISPC`) points the graphics layer at a 2 MB framebuffer reserved
at the top of SDRAM and provides the 800x480 screen in XRGB8888 to the display API and LVGL.
The console uses the same framebuffer and is not available while the display driver is enabled.
The display driver forwards brightness changes to the panel controller on McSPI1
(:kconfig:option:`CONFIG_ACX565AKM`).

LEDs
====

The LP5523 LED driver on I2C2 (:kconfig:option:`CONFIG_LP5523`) drives the keyboard backlight on
LEDs 0 to 3, 7 and 8 and the status LED on LEDs 4 (blue), 5 (green) and 6 (red). The camera flash
is an ADP1653 on I2C2 (:kconfig:option:`CONFIG_ADP1653`), with the torch limited to 50 mA.

Keyboard and power button
=========================

The TWL5030 power management chip on I2C1 scans the keyboard matrix and watches the power button
and USB VBUS, signalling changes on its interrupt line, which the TWL4030 MFD driver
(:kconfig:option:`CONFIG_MFD_TWL4030`) dispatches. With :kconfig:option:`CONFIG_INPUT` enabled, the
keypad driver reports the keys of the keymap in the board devicetree and the power button driver
reports ``INPUT_KEY_POWER``. Applications can register for USB VBUS changes with
``mfd_twl4030_add_irq_callback()``.

The lock switch and the camera key are GPIO keys reporting ``INPUT_KEY_COFFEE``,
``INPUT_KEY_CAMERA_FOCUS`` and ``INPUT_KEY_CAMERA``. The keypad slide, the front proximity sensor,
the camera lens cover and the back cover report switch events (``INPUT_EV_SW``). The TSC2005 touchscreen on McSPI1
(:kconfig:option:`CONFIG_INPUT_TSC2005`) reports touches in display coordinates.

Sensors
=======

The TSL2563 ambient light sensor on I2C2 behind the front glass is supported by the TSL2561 driver
(:kconfig:option:`CONFIG_TSL2561`). The glass attenuates the light, so the reported illuminance is
lower than the ambient light. The LIS302DL accelerometer on I2C3
(:kconfig:option:`CONFIG_LIS302DL`) reports acceleration in the axes of the chip, data ready on its
INT1 line and single taps on its INT2 line.

USB
===

The high-speed USB OTG controller with its ISP1707 PHY works as a USB device controller
(:kconfig:option:`CONFIG_UDC_OMAP3_MUSB`) under the label ``zephyr_udc0`` used by the USB device
samples, for example :zephyr:code-sample:`usb-cdc-acm`.

Bluetooth
=========

The Broadcom BCM2048 Bluetooth controller on UART2 speaks Nokia's H4+, which the
:kconfig:option:`CONFIG_BT_NOKIA_H4P` driver implements: H4 with frames padded to an even length,
negotiation and alive packets before any HCI traffic, and wakeup lines in both directions for the
controller's sleep mode. The BCM2048 is a Bluetooth 2.1 + EDR controller without LE, so it needs a
Host that supports controllers without LE.

Until Nokia's firmware script has been sent to it, the controller's ROM answers the H4+ setup and
HCI_Reset but no other command. The script is ``/lib/firmware/bcmfw.bin`` of Maemo's
``bt-firmware`` package. Nokia's license allows neither modifying it nor distributing it for
commercial purposes without their approval, so it is not part of this repository: set
:kconfig:option:`CONFIG_BT_NOKIA_H4P_FIRMWARE` to the path of a copy to build it in. The link runs
at 120000 baud, the controller's speed out of reset, as faster speeds need the UART's 13x mode.

Any of these gives the script, 9560 bytes with the MD5 sum ``37db4bcf79fa7a85f1331c6df19115b9``:

- The source of the Mer ``bcm-bt-firmware`` package on the Sailfish OS build service. Despite its
  name, the archive is not compressed:

  .. code-block:: console

     curl -LO https://build.sailfishos.org/public/source/nemo:devel:hw:ti:omap3:n900/bcm-bt-firmware/bcm-bt-firmware-0.21rc3.tar.bz2
     tar -xf bcm-bt-firmware-0.21rc3.tar.bz2
     FW=$PWD/bcm-bt-firmware-0.21rc3/BCM2048B0_002.001.011.0190.0192.c.bin
     west build -b n900 <app> -- -DCONFIG_BT_NOKIA_H4P_FIRMWARE=\"$FW\"

- ``/lib/firmware/bcmfw.bin`` on a phone running Maemo.
- A Maemo firmware image for the N900, such as
  ``RX-51_2009SE_20.2010.36-2_PR_COMBINED_MR0_ARM.bin``: ``0xFFFF -M <image> -u <dir>`` unpacks its
  root file system image, from which ``ubireader_extract_files`` of the ``ubi_reader`` Python
  package extracts ``lib/firmware/bcmfw.bin``.

The script gives the controller the placeholder address ``11:11:11:11:11:11``; a public identity
created with :c:func:`bt_id_create` before :c:func:`bt_enable` replaces it. The controller does not
support the HCI Read Encryption Key Size command, so the Host has to accept encrypted BR/EDR links
with an unknown key size.

With the console on USB (the ``cdc-acm-console`` snippet), enable
:kconfig:option:`CONFIG_USBD_CDC_ACM_WORKQUEUE`: the Host initializes on the system workqueue,
which otherwise also drains the console, and logging during the initialization then blocks it.

Charging
========

The bq24150A charger on I2C2 is enabled by default
(:kconfig:option:`CONFIG_CHARGER_BQ2415X`) and charges the battery from USB with the settings in the
board devicetree: 4.2 V, 650 mA, with a 500 mA input current limit. The bq27200 fuel gauge reports
the battery voltage, current, temperature and charge level.

The charger has no temperature input on the N900, so the board reads the battery temperature from
the fuel gauge every 10 seconds and charges only between
:kconfig:option:`CONFIG_BOARD_N900_CHARGE_TEMP_MIN` and
:kconfig:option:`CONFIG_BOARD_N900_CHARGE_TEMP_MAX` (0 to 45 degrees Celsius by default).

While the charger is active, the phone runs from USB with the battery removed;
``sys_reboot()`` restarts it into NOLO.

Programming and Debugging
*************************

The ``nolo`` runner loads images with `0xFFFF`_, the open source Maemo flasher, which asks NOLO
to load an image into RAM and start it. The image starts with a position-independent stub that
moves it to its link address, so the address NOLO loads it to does not matter.

Zephyr stops the MPU and IVA2 watchdogs of the OMAP3 that NOLO leaves running, and the watchdog
of the power management chip. Put the phone in R&D mode once with:

.. code-block:: console

   0xFFFF -R 1 -F no-omap-wd,no-ext-wd,no-lifeguard-reset

``0xFFFF -R 0`` restores the default.

Building and Running
====================

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :host-os: unix
   :board: n900
   :goals: build flash

``west flash`` waits for the phone. Power it on with USB connected; hold :kbd:`u` if it boots its
installed system instead of entering NOLO's USB mode.

The application output appears on the LCD:

.. code-block:: text

   *** Booting Zephyr OS vx.x.x-xxx-gxxxxxxxxxxxx ***
   Hello World! n900/omap3430

Holding the power button for two seconds restarts the phone into NOLO, ready for the next
``west flash`` (:kconfig:option:`CONFIG_BOARD_N900_POWER_KEY_REBOOT`). Pulling the battery returns
the phone to its installed system.

Debugging boot failures
=======================

:kconfig:option:`CONFIG_SOC_OMAP3_BOOT_STAGES` paints a colored band into NOLO's framebuffer at
each boot stage before the MMU is enabled, and a solid screen color once it is, which locates
early failures without a serial port.

.. _0xFFFF:
   https://github.com/pali/0xFFFF
