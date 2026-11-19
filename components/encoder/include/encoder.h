// encoder: the KY-040 rotary encoder with push switch (pins in board_pins.h).
// Turning changes the volume. A click toggles pause while something plays or
// is paused; a click while idle asks Home Assistant to start something (see
// ha_mqtt.h). A double click skips to the next track of a Music Assistant
// session, a triple click goes back to the previous one.
#pragma once

#include "esp_err.h"

// Set up the pins and start the poll task. Call after playback_init().
esp_err_t encoder_init(void);
