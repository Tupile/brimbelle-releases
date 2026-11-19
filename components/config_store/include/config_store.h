// config_store: configuration in internal flash.
//
// NVS holds the Wi-Fi credentials and the hashed config-page password.
// LittleFS holds config.json (see docs/config_schema.md).
//
// After config_store_init() returns ESP_OK, config_store_get() yields a valid,
// read-only, in-memory view of the configuration. The struct is owned by this
// component and stays valid for the program lifetime.
//
// Trimmed from Bugne: no podcasts, alarms, favorites, buttons or UI settings.
// The MQTT broker for Home Assistant is new here (NVS, not config.json).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Schema version this firmware understands. See docs/config_schema.md.
#define CFG_SCHEMA_VERSION 1

// Bounds. All strings are stored null-terminated within these sizes.
#define CFG_DEVICE_NAME_MAX   32
#define CFG_RADIO_NAME_MAX    48
#define CFG_URL_MAX           256
#define CFG_MAX_WEBRADIOS     32
#define CFG_MAX_EQ_BANDS      6

typedef struct {
    int  id;
    char name[CFG_RADIO_NAME_MAX];
    char url[CFG_URL_MAX];
    int  skip_preroll;  // 1: open a decoy connection first to absorb a server pre-roll ad
} config_webradio_t;

typedef struct {
    int   type;      // 1 highpass, 2 lowshelf, 3 highshelf, 4 peak: same values as audio_eq_type_t
    float hz, db, q;
} config_eq_band_t;

typedef struct {
    int volume;      // boot volume 0..100
    int volume_max;  // volume ceiling 1..100 (speaker and ear protection); default 100
    int output;      // 0 mono (default), 1 stereo, 2 left, 3 right: same values as audio_out_mode_t
    config_eq_band_t eq[CFG_MAX_EQ_BANDS];  // speaker correction, empty by default
    size_t           eq_count;
} config_audio_t;

typedef struct {
    int  schema_version;
    char device_name[CFG_DEVICE_NAME_MAX];

    config_webradio_t webradios[CFG_MAX_WEBRADIOS];
    size_t            webradio_count;

    config_audio_t audio;

    int pause_timeout_min;  // a pause longer than this stops playback; 0 never; default 60
} config_t;

// Init NVS, mount LittleFS, then load and validate config.json. A missing or
// unparseable file self-heals to defaults written back to flash. The in-memory
// config is always valid on ESP_OK.
esp_err_t config_store_init(void);

// Read-only view of the loaded configuration. Valid only after a successful
// config_store_init(). Never NULL after that.
const config_t *config_store_get(void);

// Wi-Fi credentials live in NVS, not in config.json (they are secrets).
#define CFG_WIFI_SSID_MAX 33  // 32 chars + null
#define CFG_WIFI_PASS_MAX 64  // 63 chars + null

// Slotted Wi-Fi credentials: slot 0 is the primary network, the rest are
// optional extra networks. The net layer picks the strongest visible one at
// connect time and roams between them. An empty SSID in a slot means "no
// network configured there".
#define CFG_WIFI_SLOTS 16
esp_err_t config_store_get_wifi_slot(int slot, char *ssid, size_t ssid_size,
                                     char *pass, size_t pass_size);
esp_err_t config_store_set_wifi_slot(int slot, const char *ssid, const char *pass);

// MQTT broker for Home Assistant, in NVS like the Wi-Fi credentials (the
// password is a secret). An empty url means "no broker configured".
#define CFG_MQTT_URL_MAX  128
#define CFG_MQTT_USER_MAX 64
#define CFG_MQTT_PASS_MAX 64
typedef struct {
    char url[CFG_MQTT_URL_MAX];    // "mqtt://host:1883"
    char user[CFG_MQTT_USER_MAX];
    char pass[CFG_MQTT_PASS_MAX];
} config_mqtt_t;
// Fills *out; fields never stored come back empty.
void config_store_get_mqtt(config_mqtt_t *out);
esp_err_t config_store_set_mqtt(const config_mqtt_t *in);

// Read the current config.json bytes into buf (null-terminated). Sets *out_len
// (excluding the null) if out_len is not NULL.
esp_err_t config_store_read_json(char *buf, size_t size, size_t *out_len);

// Validate JSON text against schema v1, then persist and apply it as the new
// configuration. Returns ESP_ERR_INVALID_ARG on parse or schema failure.
esp_err_t config_store_write_json(const char *json);

// Config-page password. Stored in NVS as a random salt plus the salted SHA-256
// hash of the password. The plaintext is never stored.
bool config_store_has_password(void);
esp_err_t config_store_set_password(const char *plain);
// Remove the password: the config page opens without login again.
esp_err_t config_store_clear_password(void);
// ESP_OK if it matches, ESP_ERR_NOT_FOUND if no password is set, ESP_FAIL on
// mismatch.
esp_err_t config_store_check_password(const char *plain);

#ifdef __cplusplus
}
#endif
