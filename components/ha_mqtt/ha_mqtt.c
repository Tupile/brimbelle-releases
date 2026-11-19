// ha_mqtt: Home Assistant event entity over MQTT discovery. See ha_mqtt.h.
//
// Topics, with <id> the 4-hex device id:
//   homeassistant/event/brimbelle_<id>/button/config  discovery (retained)
//   brimbelle/<id>/status   "online" / "offline" (retained, offline is the
//                           last will)
//   brimbelle/<id>/button   {"event_type":"..."} (not retained)
//   homeassistant/sensor/brimbelle_<id>/now_playing/config and .../playback/config
//   brimbelle/<id>/playback what plays, as JSON (retained, see playback_json)
#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "cJSON.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net.h"
#include "playback.h"
#include "tags.h"

static const char *TAG = "ha_mqtt";

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;  // guards s_client against a restart
static volatile bool s_connected;
static char s_status_topic[40];
static char s_button_topic[40];
static char s_playback_topic[40];
static volatile bool s_republish;  // send the playback JSON again (new connection)

// One discovery message. kind is the Home Assistant platform ("event" or
// "sensor"), object the entity's short id. The caller fills the entity keys;
// the shared device block and the availability topic are added here.
static void publish_config(esp_mqtt_client_handle_t client, const char *kind, const char *object, cJSON *o)
{
    const char *id = board_device_id();
    const config_t *c = config_store_get();
    char topic[72], uid[40], dev_id[24], name[48], url[32], ip[16];
    snprintf(topic, sizeof(topic), "homeassistant/%s/brimbelle_%s/%s/config", kind, id, object);
    snprintf(uid, sizeof(uid), "brimbelle_%s_%s", id, object);
    snprintf(dev_id, sizeof(dev_id), "brimbelle_%s", id);
    if (c->device_name[0]) strlcpy(name, c->device_name, sizeof(name));
    else snprintf(name, sizeof(name), "Brimbelle %s", id);

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = dev ? cJSON_AddArrayToObject(dev, "identifiers") : NULL;
    if (!ids) {
        cJSON_Delete(o);
        return;
    }
    cJSON_AddStringToObject(o, "unique_id", uid);
    cJSON_AddStringToObject(o, "availability_topic", s_status_topic);
    cJSON_AddItemToArray(ids, cJSON_CreateString(dev_id));
    cJSON_AddStringToObject(dev, "name", name);
    cJSON_AddStringToObject(dev, "manufacturer", "Brimbelle");
    cJSON_AddStringToObject(dev, "model", "Amped-ESP32");
    cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
    if (net_ip(ip, sizeof(ip))) {
        snprintf(url, sizeof(url), "http://%s", ip);
        cJSON_AddStringToObject(dev, "configuration_url", url);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return;
    esp_mqtt_client_enqueue(client, topic, txt, 0, 1, 1, true);
    cJSON_free(txt);
}

static void publish_discovery(esp_mqtt_client_handle_t client)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *types = o ? cJSON_AddArrayToObject(o, "event_types") : NULL;
    if (types) {
        cJSON_AddStringToObject(o, "name", "Button");
        cJSON_AddStringToObject(o, "device_class", "button");
        cJSON_AddStringToObject(o, "state_topic", s_button_topic);
        cJSON_AddItemToArray(types, cJSON_CreateString("play_request"));
        cJSON_AddItemToArray(types, cJSON_CreateString("toggle"));
        cJSON_AddItemToArray(types, cJSON_CreateString("next"));
        cJSON_AddItemToArray(types, cJSON_CreateString("previous"));
        cJSON_AddItemToArray(types, cJSON_CreateString("long_press"));
        publish_config(client, "event", "button", o);
    } else {
        cJSON_Delete(o);
    }

    // Two sensors read the same playback JSON (see playback_json).
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "name", "Now playing");
        cJSON_AddStringToObject(o, "icon", "mdi:music-note");
        cJSON_AddStringToObject(o, "state_topic", s_playback_topic);
        cJSON_AddStringToObject(o, "value_template", "{{ value_json.now_playing }}");
        cJSON_AddStringToObject(o, "json_attributes_topic", s_playback_topic);
        publish_config(client, "sensor", "now_playing", o);
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "name", "Playback");
        cJSON_AddStringToObject(o, "icon", "mdi:play-pause");
        cJSON_AddStringToObject(o, "state_topic", s_playback_topic);
        cJSON_AddStringToObject(o, "value_template", "{{ value_json.state }}");
        publish_config(client, "sensor", "playback", o);
    }
}

// The playback state as JSON, or NULL. Free with cJSON_free.
//   state        "idle", "playing" or "paused"
//   source       "none", "radio" or "music_assistant"
//   title, artist, station (station: the web radio's name)
//   now_playing  one line for display: "Artist - Title", else the title, else
//                the station; empty while idle
static char *playback_json(void)
{
    playback_status_t st;
    playback_status(&st);
    bool radio = !strcmp(st.source, "radio");
    bool idle = !strcmp(st.source, "none");
    const char *station = "";
    if (radio) {
        const config_t *c = config_store_get();
        for (size_t i = 0; i < c->webradio_count; i++) {
            if (c->webradios[i].id == st.radio_id) station = c->webradios[i].name;
        }
    }
    // Home Assistant refuses a state longer than 255 characters.
    char line[sizeof(st.artist) + sizeof(st.title) + 4];
    if (idle) line[0] = '\0';
    else if (st.title[0] && st.artist[0]) snprintf(line, sizeof(line), "%s - %s", st.artist, st.title);
    else strlcpy(line, st.title[0] ? st.title : station, sizeof(line));
    line[255] = '\0';
    tags_utf8_trim_partial(line);

    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "state", idle ? "idle" : st.paused ? "paused" : "playing");
    cJSON_AddStringToObject(o, "source", idle ? "none" : radio ? "radio" : "music_assistant");
    cJSON_AddStringToObject(o, "title", idle ? "" : st.title);
    cJSON_AddStringToObject(o, "artist", idle ? "" : st.artist);
    cJSON_AddStringToObject(o, "station", station);
    cJSON_AddStringToObject(o, "now_playing", line);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return txt;
}

// Publishes the playback JSON (retained) when it changes, and again after
// every connect. Polling once a second keeps playback free of any MQTT hook.
static void playback_task(void *arg)
{
    char *last = NULL;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_client && s_connected) {
            char *txt = playback_json();
            bool again = s_republish;
            if (txt && (again || !last || strcmp(txt, last))) {
                s_republish = false;
                esp_mqtt_client_enqueue(s_client, s_playback_topic, txt, 0, 1, 1, true);
                cJSON_free(last);
                last = txt;
            } else {
                cJSON_free(txt);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to the broker");
        publish_discovery(ev->client);
        esp_mqtt_client_enqueue(ev->client, s_status_topic, "online", 0, 1, 1, true);
        s_republish = true;
        s_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected from the broker");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "broker refused the connection (code %d): check user and password",
                     ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

// Caller holds s_lock.
static esp_err_t start_locked(void)
{
    config_mqtt_t m;
    config_store_get_mqtt(&m);
    if (!m.url[0]) {
        ESP_LOGI(TAG, "no broker configured");
        return ESP_OK;
    }
    const char *id = board_device_id();
    char client_id[24];
    snprintf(client_id, sizeof(client_id), "brimbelle-%s", id);
    snprintf(s_status_topic, sizeof(s_status_topic), "brimbelle/%s/status", id);
    snprintf(s_button_topic, sizeof(s_button_topic), "brimbelle/%s/button", id);
    snprintf(s_playback_topic, sizeof(s_playback_topic), "brimbelle/%s/playback", id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = m.url,
        .credentials.client_id = client_id,
        .credentials.username = m.user[0] ? m.user : NULL,
        .credentials.authentication.password = m.pass[0] ? m.pass : NULL,
        .session.keepalive = 30,
        // The first attempt at boot runs before Wi-Fi is up and fails: retry
        // after 3 s, not the default 10 s.
        .network.reconnect_timeout_ms = 3000,
        .session.last_will = { .topic = s_status_topic, .msg = "offline", .qos = 1, .retain = 1 },
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed (bad url?)");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "client started for %s", m.url);
    return ESP_OK;
}

esp_err_t ha_mqtt_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
        xTaskCreate(playback_task, "ha_mqtt_pb", 4096, NULL, 3, NULL);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_client ? ESP_OK : start_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ha_mqtt_restart(void)
{
    if (!s_lock) return ha_mqtt_start();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_client) {
        // A clean disconnect does not send the last will: say offline first.
        if (s_connected) esp_mqtt_client_publish(s_client, s_status_topic, "offline", 0, 1, 1);
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
    esp_err_t err = start_locked();
    xSemaphoreGive(s_lock);
    return err;
}

bool ha_mqtt_connected(void)
{
    return s_connected;
}

void ha_mqtt_button_event(const char *event_type)
{
    // The lock is held for a moment by the playback task and for seconds by
    // a restart: wait out the first, drop the event on the second.
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (s_client && s_connected) {
        char msg[48];
        snprintf(msg, sizeof(msg), "{\"event_type\":\"%s\"}", event_type);
        esp_mqtt_client_enqueue(s_client, s_button_topic, msg, 0, 1, 0, true);
    }
    xSemaphoreGive(s_lock);
}
