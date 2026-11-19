# Hardware: Sonocotta Amped-ESP32, ESP32-S3 variant

ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB octal PSRAM, built-in antenna), DAC
PCM5100A on plain I2S (no control bus), class D amplifier TPA3118D2 (stereo,
each channel bridged), external supply 5 to 26 V on a 5.5 x 2.5 mm barrel
jack, speaker outputs on screw terminals. Verified against the rev K0
schematic in sonocotta/esp32-audio-dock (hardware/4-amped-esp32/rev-k0).

## GPIO map

| Function | Signal | GPIO |
| --- | --- | --- |
| I2S to PCM5100A | BCLK (net I2S0_CLK, DAC BCK) | IO14 |
| | WS / LRCK (net I2S0_WS) | IO15 |
| | DOUT (net I2S0_DATA, DAC DIN) | IO16 |
| | MCLK | none, the DAC clocks itself from BCK |
| TPA3118 enable | net DAC_EN, active HIGH (high plays, low mutes) | IO17 (rev J and later) |
| Native USB | D- / D+ | IO19 / IO20 |
| Octal PSRAM | reserved | IO35, IO36, IO37 |
| RGB LED header | data | IO9 |
| IR receiver header | in | IO7 |
| KY-040 encoder (on the SPI header) | CLK / DT / SW | IO12 / IO13 / IO5 |
| SPI header (OLED, W5500) | unused, rest of the header | IO10, 11, 6, 38, 47, 48 |
| Free header pins | | IO1, IO2, IO42 |

## Amplifier enable

IO17 drives the gate of an AO3400A MOSFET that pulls the TPA3118 MUTE pin
low. Schematic notes: "DAC_EN -> high - PLAY", "MUTE -> high - HIGH-Z". A
100 k pull-down keeps the amp muted at reset and while the GPIO is not
driven, so the board boots silent. The PCM5100A XSMT pin is not tied to this
net: muting the amp does not mute the DAC.

The amp gain is fixed at 20 dB (R8 5.6 k) and the PLIMIT clipper is off, so
the firmware volume ceiling is the only protection against clipping. The
PBTL solder jumpers SJ1 and SJ2 are left open: by default the speaker is on
one channel (OUTL) and the firmware mixes to mono. See "Stereo" below for two
speakers.

## Rotary encoder

A KY-040 module: CLK on IO12, DT on IO13, SW on IO5, GND to GND. The module's
"+" pin goes to 3.3 V or stays unconnected, never to 5 V: it feeds the
module's pull-ups and the GPIOs are not 5 V tolerant. The firmware enables the
internal pull-ups on the three pins, so the encoder works either way.

CLK and DT feed a hardware pulse counter (PCNT) in quadrature mode, 4 counts
per detent. Turning changes the volume by 2 points per detent
(ENC_VOLUME_STEP in components/encoder/encoder.c). A click toggles pause
while a source plays or is paused. A double click (two presses within 400 ms)
skips to the next track of a Music Assistant session, a triple click goes
back to the previous one; the single and double clicks therefore act 400 ms
after their last press. A press held 1.5 s fires the
`long_press` event of the Home Assistant entity and changes nothing on the
speaker (a short beep confirms it when nothing plays). A click while idle does nothing on the
speaker: it fires the `play_request` event of the Home Assistant entity (see
components/ha_mqtt/include/ha_mqtt.h). The same component publishes what
plays to Home Assistant as two sensors.

IO12 and IO13 are also CLK and MISO of the Sonocotta OLED module on the same
header: the encoder and that screen cannot share these pins.

## Speaker and supply

Visaton FRS 8 M, 8 ohm (30 W RMS), on OUTL.

Supply: APD DA-40B19 adapter, 19 V 2.1 A, 5.5 x 2.5 mm barrel. Under 19 V the
TPA3118 can deliver about 20 W on 8 ohm before clipping, well above a living
room level. The amp gain is fixed (20 dB) and PLIMIT is off, so the firmware
volume cap is the only protection. The DAC gives 2.1 Vrms full scale, so
0 dBFS would be 21 Vrms at the amp output: clipping starts near -4 dBFS on
19 V. AUDIO_GAIN_MAX_DB (components/audio/audio.c) is the ceiling at volume
100; -9 dB (about 7 W on a sine) was set on the bench on 2026-11-09: clean
440 Hz sine at volume 100, no pop on amp enable/disable, total silence at
idle. On 2026-11-11 the user found volume 100 too quiet and it was raised to
-6 dB (about 14 W on a sine). Never raise it above -6 dB.

## Stereo

The output mode is the `audio.output` setting (web page, "Output"), mono by
default. Not yet tested on the bench with two speakers.

The I2S WS polarity is inverted in audio.c (`ws_pol`): with the standard
polarity the left channel came out of the terminal marked R (bench,
2026-11-13: one speaker on L, "Tone L" silent and "Tone R" heard).

- One board, two speakers (`stereo`): the second 8 ohm speaker goes on OUTR.
  SJ1 and SJ2 stay open. The -6 dB ceiling applies to each channel: about 14 W
  per channel on a sine, 28 W in total, within the 40 W of the 19 V 2.1 A
  adapter. Check which side is which with the "Tone L" and "Tone R" buttons
  (`POST /api/test` with `"side":"left"` or `"right"`).
- Two units as a pair (`left` on one, `right` on the other): each unit keeps
  its single speaker on OUTL and plays one channel of the stream. The two must
  be in one synchronized group in Music Assistant. Web radios are not
  synchronized between units. Each unit keeps its own volume.

USB alone is not a usable supply. USB VBUS reaches the +5 V rail through a
1N5819 Schottky (about 4.6 V) and the 3.3 V regulator is an AMS1117, which
then drops out on every Wi-Fi transmit burst. Measured on the bench
(2026-11-09, same spot, same RSSI): 31 % packet loss and 51 ms RTT on USB
only, 1 % and 11 ms with the barrel supply connected. Always power the board
from the barrel jack; the USB cable may stay plugged (the diode prevents any
backfeed into the host).

## Console and flashing

No USB-UART bridge: the console is the USB Serial/JTAG peripheral
(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG). esptool flashes through the same USB
port. Hold BOOT (IO0) at power-up only if the automatic reset fails.

Any serial client that drives DTR low while RTS is high
resets the chip (that is the EN pulse of the USB Serial/JTAG peripheral), so
monitors must leave both lines asserted, as pyserial does by default.

## Wi-Fi notes from the bench

The station joins the strongest access point of a configured SSID: net.c asks
for an all-channel scan sorted by signal. The ESP-IDF default (fast scan)
joins the first access point that answers, and with a box and a mesh pod
sharing one SSID it was seen joining the pod at -94 dBm next to the box at
-51 dBm. With several networks configured, the boot scan that orders them also
gives the strongest access point, and the first connect goes straight to it;
every later connect scans all channels again. Measured on 2026-11-10 with two
networks configured: 20 boots of 20 join the strongest access point, with no
connect retry, and the address is obtained 6.5 s after boot.
While connected on a weak signal (-70 dBm or less) net.c looks for an access
point at least 8 dB stronger, of the same SSID or of another configured one.
With several networks configured, net.c reorders them by signal at boot, so
near-equal signals can flip the choice between boots: when one AP is known
bad, configure only the good one.

When the link drops repeatedly, net.c runs a diagnostic scan after 4 and then
every 12 consecutive failures and logs every beacon it hears (SSID, BSSID,
channel, RSSI) on the console. /api/scan returns bssid and channel for this
reason.

Deaf radio at the desk (2026-11-09 and 2026-11-10): on the user's desk, wired
to the PC over USB, the radio went deaf in episodes of 10 to 90 s. Scans
returned no network at all (up to 14 of 24), even for a -46 dBm AP, and the
link dropped with a beacon timeout. Moved away from the desk with the USB
cable unplugged: 103 of 103 scans hear the APs and the link holds. The cause
is the desk location or the PC's USB cable (not separated), not the firmware
or the board. A scan that returns 0 networks is the signature: check it
before changing Wi-Fi code.

Scan dwell: the ESP-IDF default of 120 ms per channel often misses the AP in
the scan that esp_wifi_connect runs at boot (reason 201), on a channel shared
by three APs. net.c sets 300 ms (SCAN_ACTIVE_MAX_MS). Measured over reboots at
-73 dBm: 29 of 74 boots needed a retry at 120 ms, 5 of 60 at 300 ms.

Flash mode: QIO was tried (the chip is quad capable, the board boots and logs
"flash io: qio"). No gain was measured, so the build stays on DIO.
