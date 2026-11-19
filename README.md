# Brimbelle

Brimbelle is open-source firmware for a connected speaker with no screen. It
runs on the Sonocotta Amped-ESP32 (ESP32-S3, PCM5100A DAC, TPA3118 amplifier)
and plays two kinds of sources:

- Music Assistant, over Sendspin, its native playback protocol. The speaker
  shows up as a player and can be grouped and synchronized with others.
- Web radios, streamed over HTTP and HTTPS: MP3, AAC, FLAC and Ogg (Opus or
  Vorbis).

One knob does the daily work, a small web page does the setup, and the
speaker updates itself over Wi-Fi. It is a "Bugne without the screen": the
firmware reuses components of
[Bugne](https://github.com/Tupile/bugne-releases), see UPSTREAM.md.

The firmware was developed with substantial help from AI coding assistants.

## Hardware

| Part | Used here |
| --- | --- |
| Board | Sonocotta Amped-ESP32, ESP32-S3 variant (8 MB flash, 8 MB PSRAM), rev J or later |
| Speaker | One 8 ohm full-range driver on OUTL (a Visaton FRS 8 M on the bench). A second one on OUTR for stereo |
| Supply | 19 V on the barrel jack. USB alone is not a usable supply |
| Knob | KY-040 rotary encoder: CLK on IO12, DT on IO13, SW on IO5 |

The full pin map, the amplifier notes and the bench measurements are in
docs/hardware.md.

## The knob

| Gesture | Effect |
| --- | --- |
| Turn | Volume, 2 points per detent |
| Click | Pause or resume. While idle: fires `play_request` in Home Assistant |
| Double click | Next track (Music Assistant only) |
| Triple click | Previous track (Music Assistant only) |
| Hold 1.5 s | Fires `long_press` in Home Assistant, nothing changes on the speaker |

Presses count as one gesture when they are less than 400 ms apart, so a
single or a double click acts 400 ms after its last press.

## Features

**Audio**

- Mono by default: the left and right channels are mixed for one speaker.
  The output mode is a setting: `mono`, `stereo` (two speakers on one board),
  `left` or `right` (one unit as one side of a synchronized pair).
- Sound correction: up to 6 filters (high-pass, shelves, peaks) applied to
  everything that plays, set from the web page.
- A volume ceiling, and a fixed gain limit in the firmware that keeps the
  amplifier out of clipping.
- A web radio that drops reconnects by itself. A pause longer than a set
  time (60 min by default) ends in a stop.

**Network and web page**

- First setup through the speaker's own Wi-Fi hotspot and a captive portal.
- Several Wi-Fi networks can be stored. The speaker joins the strongest
  access point and moves to a better one when the signal gets weak.
- The web page is embedded in the firmware and can be protected by a
  password. It has these cards: status, playback, Wi-Fi, speaker, sound correction, Home
  Assistant (MQTT), web radios, firmware, password and maintenance (logs,
  crash report, reboot).
- mDNS name: `brimbelle-XXXX.local`, where XXXX is the device id.

**Home Assistant**

With an MQTT broker configured, the speaker announces itself by MQTT
discovery:

- An event entity for the knob, with the events `play_request`, `toggle`,
  `next`, `previous` and `long_press`. An automation can start a playlist on
  a click while the speaker is idle.
- Two sensors: "Now playing" (title, artist, station and source as
  attributes) and "Playback" (idle, playing, paused).

**Updates**

- From the web page: install the latest GitHub release, or upload a local
  `.bin` file.
- Two firmware slots. A new image is confirmed after 30 s of healthy uptime;
  the bootloader goes back to the previous one if it crashes before that.
- An image that is not a Brimbelle firmware is refused.

## Install

The easiest way is the web flasher, with Chrome, Edge or Opera on a desktop:

<https://tupile.github.io/brimbelle-releases/tools/web-flasher/>

Plug the board into the computer by its native USB port, click the button
and pick the port of the board.

Without a browser: download `brimbelle-flash.zip` from the
[latest release](https://github.com/Tupile/brimbelle-releases/releases/latest),
unzip it and run `./flash.sh` (needs esptool; `--erase` wipes the stored
Wi-Fi and settings first).

Each release carries two files:

| File | Use |
| --- | --- |
| `brimbelle.bin` | Update image, for the "Firmware" card of the web page |
| `brimbelle-flash.zip` | First flash over USB: bootloader, partition table, firmware and `flash.sh` |

## First setup

1. Power the board from the barrel jack.
2. With no Wi-Fi stored, the speaker raises the hotspot
   `Brimbelle-Setup-XXXX`. Its password is specific to each board and is
   printed in the boot log, on the USB console (`idf.py monitor` or any
   serial terminal).
3. Join the hotspot and open <http://192.168.4.1/>. Add your Wi-Fi network.
4. The speaker joins the network. Open `http://brimbelle-XXXX.local/` to
   set a password for the page, add web radios, name the speaker and set the
   MQTT broker.
5. In Music Assistant, the speaker appears as a Sendspin player.

## Build from source

With ESP-IDF 6.1 or later:

    idf.py set-target esp32s3
    idf.py build
    idf.py -p /dev/ttyACM0 flash monitor

The dependencies are fetched by the IDF component manager
(main/idf_component.yml, versions locked in dependencies.lock). The Sendspin
library is pinned and patched at build time (patches/).

Host tests, plain C with no ESP-IDF: `test/host/run.sh`.

## Repository layout

| Path | Content |
| --- | --- |
| `main/` | Boot sequence |
| `components/audio` | I2S output, gain, mono mix, sound correction |
| `components/decode` | MP3, FLAC, AAC and Ogg decoding, tags |
| `components/source_stream` | Web radio source |
| `components/source_sendspin` | Music Assistant (Sendspin) source |
| `components/playback` | Play, stop and pause logic shared by the sources |
| `components/encoder` | The knob |
| `components/ha_mqtt` | Home Assistant entities over MQTT |
| `components/net` | Wi-Fi manager, setup hotspot, roaming, mDNS |
| `components/web_config` | Web page and its HTTP API, OTA |
| `components/config_store` | Settings (docs/config_schema.md) |
| `components/board` | Pin map and device identity |
| `addon/ma_single_stream` | Optional Home Assistant add-on, see its README |
| `tools/` | `flash.sh` and the web flasher page |
| `docs/` | Hardware notes and the settings schema |

This repository is a snapshot of the source at each release: it carries no
development history.

## Limits

- Stereo with two speakers on one board is implemented but was not tested
  with two speakers.
- The longest runs checked are a few hours. A 24 h soak was not done.
- The hotspot password is only shown on the USB console.

## License

MIT, see LICENSE. The vendored decoders in components/decode (dr_mp3,
dr_flac, minimp4) keep their own licenses, stated in each file.
