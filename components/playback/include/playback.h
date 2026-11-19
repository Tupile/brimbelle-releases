// playback: the one playback worker of the device.
//
// Bugne runs its play requests on the UI component's worker task. Brimbelle
// has no UI, so this small component owns that role: a single-slot request
// queue, a worker task that runs the stream source (with the same reconnect
// policy as Bugne), and a status snapshot for the web API. Music Assistant
// (source_sendspin) plays on its own task; this component only yields to it
// and ends its session when a local play or stop is requested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;        // something is playing or engaged (radio, retrying, or an MA session)
    bool paused;
    bool retrying;      // radio stream died, reconnect pending
    char source[12];    // "none", "radio" or "sendspin"
    int  radio_id;      // configured webradio id when source is "radio", else 0
    char title[128];    // ICY "now playing" (radio) or track title (sendspin)
    char artist[128];   // sendspin only
} playback_status_t;

// Create the request queue and the worker task.
esp_err_t playback_init(void);

// Play the configured webradio with this id. Stops whatever plays now (a
// Music Assistant session included). ESP_ERR_NOT_FOUND for an unknown id.
esp_err_t playback_play_radio(int id);

// Stop everything: mute at once, end the radio stream (no reconnect) and ask
// Music Assistant to stop its session.
void playback_stop(void);

// Pause or resume the current source (radio: holds the decode loop; sendspin:
// transport command to the server). No-op when nothing plays.
void playback_toggle(void);

// Whether a source still holds the audio output. Used by the OTA and reboot
// handlers to wait for a clean stop before touching flash.
bool playback_active(void);

void playback_status(playback_status_t *out);

#ifdef __cplusplus
}
#endif
