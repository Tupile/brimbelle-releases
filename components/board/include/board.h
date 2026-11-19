// board: pin map and device identity.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compute the device ID from the MAC. Call once at boot.
esp_err_t board_init(void);

// 4 hex chars from the last 2 bytes of the MAC, for example "A1B2". Drives the
// mDNS hostname, the setup AP SSID, the AP password seed, and the Sendspin name.
const char *board_device_id(void);

// Per-device WPA2 password for the setup AP, derived from a hash of the full
// MAC (not a hardcoded shared secret). Stable across reboots, 12 hex chars.
const char *board_ap_password(void);

// Human-readable reason for the last chip reset ("POWERON", "PANIC", ...).
// Logged at boot and served in /api/status.
const char *board_reset_reason_name(void);

#ifdef __cplusplus
}
#endif
