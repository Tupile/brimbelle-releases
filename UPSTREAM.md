# Upstream: Bugne

Brimbelle reuses components of the Bugne firmware (Tupile/bugne). This file
records where each one comes from, so a later resync is a `diff -r` against
that commit, not an archaeology project.

Base commit: Tupile/bugne `1237bd0` (Release v1.20.1, 2026-11-01).

| Component | Status | Deliberate differences |
| --- | --- | --- |
| `components/logstore` | copied as is | none |
| `components/net` | copied | identity strings (`brimbelle-XXXX`, `Brimbelle-Setup-XXXX`, `_brimbelle._tcp`); `net_memo_peers` and `net_peer_t` removed (no voice memos); `net_ap_t` gains `bssid` and `channel`, every scanned beacon is logged, and a diagnostic scan runs during reconnect storms (see docs/hardware.md); the setup AP password is logged when the provisioning AP starts with no stored credentials (no screen to show it) |
| `components/config_store` | trimmed | schema keeps `device.name`, `webradios[]`, `audio.volume`, `audio.volume_max`; NVS namespace `brimbelle`; no podcast, alarm, favorite, button, UI, HA, game or usage state |
| `components/board` | rewritten | Amped-ESP32 S3 pin map, no I2C bus |
| `components/web_config` | reduced | routes: `/`, `/login`, `/api/password`, `/api/scan`, `/api/wifi`, `/api/mqtt`, `/api/config`, `/api/logs`, `/api/coredump[/erase]`, `/api/ota`, `/api/ghota[/check,/status]`, `/api/status`, `/api/test`, `/api/playback`, `/api/reboot`; `/api/playback` actions are `radio`, `stop`, `toggle`, `volume` (no SD, podcast, seek or sleep); OTA and reboot stop playback through `playback` instead of `ui_remote`; OTA refuses an image whose `project_name` is not `brimbelle`; GitHub channel `Tupile/brimbelle-releases`; own small `www/index.html` (12.8 KB: status, playback, Wi-Fi, speaker, web radios, firmware, password, maintenance) |
| `components/audio` | rewritten | `audio.h` keeps Bugne's API minus `audio_record_*` and the I2C bus argument; `audio.c` drives the PCM5100A over `i2s_std` (TX only, 32-bit slots, no MCLK) with a software gain and an output mode, mono downmix by default (`audio_mix.c`, host-tested) instead of ES8311 registers; amp enable IO17 active high; adds `audio_test_tone`. `audio_arbiter.c` copied as is, enum trimmed to NONE, STREAM, SENDSPIN, TONE |
| `main` | rewritten | boot order: logstore, pm, config_store, board, audio, net, web_config, source_stream, playback, source_sendspin, ha_mqtt, encoder |
| `components/source_sendspin` | copied | names (`BrimbellePlayerListener`, product and manufacturer "Brimbelle", fallback player name "Brimbelle <id>"); sendspin-cpp pinned to 0.8.0 as in Bugne |
| `components/source_stream` | copied as is | none (`rf_meta` included) |
| `components/decode` | copied | `art.c` and `art.h` dropped (no screen, no esp_jpeg): the ID3 APIC and FLAC PICTURE blocks are ignored; `tags.c` and `adts.c` kept |
| `components/http_util` | copied as is | none |
| `components/playback` | new | the play worker that lives in Bugne's `ui` component (play_task, ui_play, ui_stop, ui_status): same single-slot request queue, Sendspin takeover wait and stream reconnect backoff, reduced to webradios and Music Assistant |
| `components/ha_mqtt` | new | MQTT client (`espressif/mqtt`) that announces the knob to Home Assistant as an event entity, and what plays as two sensors, by MQTT discovery; Bugne has no MQTT (its `ha_client` is a REST client and was not copied) |
| `components/encoder` | new | KY-040 rotary encoder (PCNT) for volume and pause; `buttons.c` and `buttons.h` copied as is from Bugne's `components/ui` (debounce); `enc_logic.c` is new |
| `test/host` | pattern | `run.sh` rebuilt from Bugne's; tests copied as is for `url_escape`, `http_util` (with its `esp_http_client` stub), `adts`, `tags`, `rf_meta`; `test_buttons.c` copied without the pin and action tests; `test_audio_mix.c` and `test_enc_logic.c` are new |
| `patches/sendspin-cpp-0.8.0-esp-httpd-stack.patch` | copied as is | applied by the root CMakeLists.txt once sendspin-cpp is a dependency |
| `sdkconfig.defaults` | adapted | 8 MB flash, DIO, USB Serial/JTAG console; no LVGL, FATFS or QIO; PSRAM tuning kept |
| `partitions.csv` | rewritten | 8 MB layout, two 3 MB OTA slots, 512 KB littlefs |

