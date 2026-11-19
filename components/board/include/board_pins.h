// board_pins: the fixed GPIO map of the Sonocotta Amped-ESP32 (ESP32-S3
// variant, rev J and K). Single source of truth for pin numbers. Do not
// hardcode GPIO numbers elsewhere. See docs/hardware.md.
#pragma once

// I2S to the PCM5100A DAC. No MCLK: the DAC clocks itself from BCK.
#define BOARD_I2S_BCLK_GPIO  14   // net I2S0_CLK,  PCM5100A BCK
#define BOARD_I2S_WS_GPIO    15   // net I2S0_WS,   PCM5100A LRCK
#define BOARD_I2S_DOUT_GPIO  16   // net I2S0_DATA, PCM5100A DIN

// TPA3118 amplifier enable (net DAC_EN). ACTIVE HIGH: high plays, low mutes
// (the GPIO drives a MOSFET on the amp MUTE pin; a 100k pull-down keeps the
// amp muted at reset and while the pin is not driven).
#define BOARD_AMP_EN_GPIO    17

// KY-040 rotary encoder with push switch, wired on the SPI header. All three
// are switched to GND; the firmware enables the internal pull-ups.
#define BOARD_ENC_CLK_GPIO   12
#define BOARD_ENC_DT_GPIO    13
#define BOARD_ENC_SW_GPIO    5

// Headers, unused for now: RGB LED data (9), IR receiver (7), the rest of the
// SPI header (10, 11, 6, 38, 47, 48) and the free pins IO1, IO2, IO42.
// GPIO 35, 36, 37 are the octal PSRAM. GPIO 19/20 are the native USB.
