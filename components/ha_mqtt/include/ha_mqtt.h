// ha_mqtt: the speaker in Home Assistant over MQTT discovery: the knob as an
// event entity, and what plays as two sensors. The broker (url, user, password) comes from config_store; with no
// url nothing starts. The event entity fires one of five event types:
//   "play_request"  click while idle: nothing happens on the speaker, a Home
//                   Assistant automation is expected to start something
//   "toggle"        click while playing or paused (already handled locally)
//   "next"          double click on a Music Assistant session (handled locally)
//   "previous"      triple click on a Music Assistant session (handled locally)
//   "long_press"    button held 1.5 s, whatever plays: nothing happens on the
//                   speaker (a short beep if idle), a Home Assistant
//                   automation decides (Bluetooth pairing on the bridge)
//
// Two sensors go with it: "Now playing" (one line, with title, artist,
// station and source as attributes) and "Playback" (idle, playing, paused).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Start the client if a broker is configured. Call once at boot.
esp_err_t ha_mqtt_start(void);

// Stop the client and start it again with the stored settings (after
// config_store_set_mqtt). Blocks for up to a few seconds.
esp_err_t ha_mqtt_restart(void);

bool ha_mqtt_connected(void);

// Fire the button event. Never blocks; dropped when the broker is not
// connected (a late "play_request" would start music long after the press).
void ha_mqtt_button_event(const char *event_type);
