// web_config: the configuration web page and its REST endpoints.
//
// Served by esp_http_server with embedded HTML/CSS/JS (no CDN, the device may
// be offline). Reads and writes config.json. Lets the user manage Wi-Fi, the
// device name, the web radios and the volume ceiling, and update the firmware.
// Behind a login: password hashed on flash, session cookie.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the config web server. Called once the network is up (station
// connected, or the setup AP during provisioning).
esp_err_t web_config_start(void);

#ifdef __cplusplus
}
#endif
