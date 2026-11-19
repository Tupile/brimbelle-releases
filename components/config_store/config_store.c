// config_store: NVS + LittleFS, plus loading and validating config.json.
// Trimmed from Bugne (see UPSTREAM.md): device name, web radios and audio only.
#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "psa/crypto.h"

static const char *TAG = "config_store";

#define LFS_BASE_PATH    "/littlefs"
#define LFS_PARTITION    "littlefs"
#define CONFIG_PATH      LFS_BASE_PATH "/config.json"
#define CONFIG_TMP_PATH  LFS_BASE_PATH "/config.tmp"
#define CONFIG_FILE_MAX  32768  // hard cap on config.json size

// The in-memory config lives in PSRAM instead of internal .bss (the scarce
// resource). The alias macro keeps every s_config.x call site plain.
static config_t *s_cfg;
#define s_config (*s_cfg)
static bool s_ready;
static SemaphoreHandle_t s_lock;

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// audio.output values, indexed by config_audio_t.output.
static const char *const k_outputs[] = { "mono", "stereo", "left", "right" };

// audio.eq[].type values, indexed by config_eq_band_t.type (0 is not a filter).
static const char *const k_eq_types[] = { "", "highpass", "lowshelf", "highshelf", "peak" };

static void set_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->schema_version = CFG_SCHEMA_VERSION;
    c->device_name[0] = '\0';  // empty -> consumers fall back to "Brimbelle <id>"
    c->webradio_count = 0;
    c->audio.volume = 50;
    c->audio.volume_max = 100;
    c->pause_timeout_min = 60;
}

// Ids must be unique and positive: the playback API addresses a webradio BY ID
// (so a web reorder cannot repoint it), and the fallback id for an entry that
// has none is positional, so a hand-edited config can collide. Renumbers only
// the offenders, so every well-formed id survives.
static void dedup_ids(int *ids, size_t n)
{
    int max = 0;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] > max) max = ids[i];
    }
    for (size_t i = 0; i < n; i++) {
        bool dup = (ids[i] <= 0);
        for (size_t j = 0; !dup && j < i; j++) {
            if (ids[j] == ids[i]) dup = true;
        }
        if (dup) ids[i] = ++max;
    }
}

// Overlay parsed JSON onto an already-defaulted config. Missing fields keep
// their defaults. All strings are bounded. An entry without its required URL is
// skipped. Treat every value as untrusted.
static void load_from_json(config_t *c, const cJSON *root)
{
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(dev)) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            strlcpy(c->device_name, name->valuestring, sizeof(c->device_name));
        }
    }

    const cJSON *radios = cJSON_GetObjectItemCaseSensitive(root, "webradios");
    if (cJSON_IsArray(radios)) {
        // A present array REPLACES the list; an absent key keeps the defaults.
        c->webradio_count = 0;
        int n = cJSON_GetArraySize(radios);
        for (int i = 0; i < n && c->webradio_count < CFG_MAX_WEBRADIOS; i++) {
            const cJSON *it = cJSON_GetArrayItem(radios, i);
            if (!cJSON_IsObject(it)) continue;
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(it, "url");
            if (!cJSON_IsString(url) || !url->valuestring || url->valuestring[0] == '\0') continue;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            config_webradio_t *w = &c->webradios[c->webradio_count++];
            w->id = cJSON_IsNumber(id) ? id->valueint : (int)c->webradio_count;
            strlcpy(w->name, (cJSON_IsString(nm) && nm->valuestring) ? nm->valuestring : "", sizeof(w->name));
            strlcpy(w->url, url->valuestring, sizeof(w->url));
            // The web form posts checkbox values as a number; accept a string too.
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(it, "skip_preroll");
            int spv = cJSON_IsNumber(sp) ? sp->valueint
                    : (cJSON_IsString(sp) && sp->valuestring) ? atoi(sp->valuestring) : 0;
            w->skip_preroll = spv ? 1 : 0;
        }
        int ids[CFG_MAX_WEBRADIOS];
        for (size_t i = 0; i < c->webradio_count; i++) ids[i] = c->webradios[i].id;
        dedup_ids(ids, c->webradio_count);
        for (size_t i = 0; i < c->webradio_count; i++) c->webradios[i].id = ids[i];
    }

    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (cJSON_IsObject(audio)) {
        const cJSON *vol = cJSON_GetObjectItemCaseSensitive(audio, "volume");
        if (cJSON_IsNumber(vol)) c->audio.volume = clampi(vol->valueint, 0, 100);
        const cJSON *vmax = cJSON_GetObjectItemCaseSensitive(audio, "volume_max");
        if (cJSON_IsNumber(vmax)) c->audio.volume_max = clampi(vmax->valueint, 1, 100);
        const cJSON *outp = cJSON_GetObjectItemCaseSensitive(audio, "output");
        if (cJSON_IsString(outp)) {
            c->audio.output = 0;  // an unknown value is mono
            for (int i = 0; i < 4; i++) {
                if (strcmp(outp->valuestring, k_outputs[i]) == 0) c->audio.output = i;
            }
        }
        const cJSON *eq = cJSON_GetObjectItemCaseSensitive(audio, "eq");
        if (cJSON_IsArray(eq)) {
            c->audio.eq_count = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, eq) {
                if (c->audio.eq_count >= CFG_MAX_EQ_BANDS) break;
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(it, "type");
                const cJSON *hz = cJSON_GetObjectItemCaseSensitive(it, "hz");
                const cJSON *db = cJSON_GetObjectItemCaseSensitive(it, "db");
                const cJSON *q = cJSON_GetObjectItemCaseSensitive(it, "q");
                int t = 0;
                for (int i = 1; i < 5 && cJSON_IsString(type); i++) {
                    if (strcmp(type->valuestring, k_eq_types[i]) == 0) t = i;
                }
                if (!t || !cJSON_IsNumber(hz)) continue;  // an unknown filter is dropped
                config_eq_band_t *b = &c->audio.eq[c->audio.eq_count++];
                b->type = t;
                b->hz = (float)hz->valuedouble;
                b->db = cJSON_IsNumber(db) ? (float)db->valuedouble : 0.0f;
                b->q = cJSON_IsNumber(q) ? (float)q->valuedouble : 0.707f;
            }
        }
    }

    const cJSON *pb = cJSON_GetObjectItemCaseSensitive(root, "playback");
    const cJSON *pt = cJSON_IsObject(pb) ? cJSON_GetObjectItemCaseSensitive(pb, "pause_timeout_min") : NULL;
    if (cJSON_IsNumber(pt)) c->pause_timeout_min = clampi(pt->valueint, 0, 1440);
}

// A float as the 3-decimal number the user typed (0.707, not 0.7070000171661377).
static double round3(float v) { return round((double)v * 1000.0) / 1000.0; }

// Serialize a config to config.json. Writes a temp file then renames it, so a
// crash mid-write never corrupts the live config.
static esp_err_t save_to_disk(const config_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    if (!cJSON_AddNumberToObject(root, "schema_version", c->schema_version)) goto no_mem;
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    if (!dev || !cJSON_AddStringToObject(dev, "name", c->device_name)) goto no_mem;

    cJSON *radios = cJSON_AddArrayToObject(root, "webradios");
    if (!radios) goto no_mem;
    for (size_t i = 0; i < c->webradio_count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o || !cJSON_AddItemToArray(radios, o)) {
            cJSON_Delete(o);
            goto no_mem;
        }
        if (!cJSON_AddNumberToObject(o, "id", c->webradios[i].id) ||
            !cJSON_AddStringToObject(o, "name", c->webradios[i].name) ||
            !cJSON_AddStringToObject(o, "url", c->webradios[i].url) ||
            !cJSON_AddNumberToObject(o, "skip_preroll", c->webradios[i].skip_preroll)) goto no_mem;
    }

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    if (!audio ||
        !cJSON_AddNumberToObject(audio, "volume", c->audio.volume) ||
        !cJSON_AddNumberToObject(audio, "volume_max", c->audio.volume_max) ||
        !cJSON_AddStringToObject(audio, "output", k_outputs[c->audio.output])) goto no_mem;
    cJSON *eq = cJSON_AddArrayToObject(audio, "eq");
    if (!eq) goto no_mem;
    for (size_t i = 0; i < c->audio.eq_count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o || !cJSON_AddItemToArray(eq, o)) {
            cJSON_Delete(o);
            goto no_mem;
        }
        if (!cJSON_AddStringToObject(o, "type", k_eq_types[c->audio.eq[i].type]) ||
            !cJSON_AddNumberToObject(o, "hz", round3(c->audio.eq[i].hz)) ||
            !cJSON_AddNumberToObject(o, "db", round3(c->audio.eq[i].db)) ||
            !cJSON_AddNumberToObject(o, "q", round3(c->audio.eq[i].q))) goto no_mem;
    }

    cJSON *pb = cJSON_AddObjectToObject(root, "playback");
    if (!pb || !cJSON_AddNumberToObject(pb, "pause_timeout_min", c->pause_timeout_min)) goto no_mem;

    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;

    // Refuse to write a config larger than load_config accepts: the next boot
    // would reject it and fall back to defaults. Keep the good on-disk config.
    if (strlen(txt) >= CONFIG_FILE_MAX) {
        ESP_LOGE(TAG, "config too large to persist (%u >= %d), not written",
                 (unsigned)strlen(txt), CONFIG_FILE_MAX);
        cJSON_free(txt);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    FILE *f = fopen(CONFIG_TMP_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", CONFIG_TMP_PATH);
        err = ESP_FAIL;
    } else {
        size_t len = strlen(txt);
        size_t wr = fwrite(txt, 1, len, f);
        // fclose can still fail (the final flush hits a full LittleFS): promoting
        // the temp file then would replace a good config with a truncated one.
        bool closed = (fclose(f) == 0);
        if (wr != len || !closed) {
            ESP_LOGE(TAG, "short write to %s", CONFIG_TMP_PATH);
            remove(CONFIG_TMP_PATH);
            err = ESP_FAIL;
        } else if (rename(CONFIG_TMP_PATH, CONFIG_PATH) != 0) {
            ESP_LOGE(TAG, "rename to %s failed", CONFIG_PATH);
            err = ESP_FAIL;
        }
    }

    cJSON_free(txt);
    return err;

no_mem:
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
}

// Load config.json into s_config. Always leaves a valid in-memory config.
// A missing or empty file self-heals by writing defaults back to flash. A file
// that exists but cannot be used (too large, unparseable, unknown
// schema_version) is KEPT: it is the user's data. Defaults are used in memory
// and the error is logged; the file stays available for download.
static esp_err_t load_config(void)
{
    set_defaults(&s_config);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "no config.json, writing defaults");
        return save_to_disk(&s_config);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        ESP_LOGE(TAG, "config.json is empty, writing defaults");
        return save_to_disk(&s_config);
    }
    if (sz > CONFIG_FILE_MAX) {
        fclose(f);
        ESP_LOGE(TAG, "config.json size %ld over the %d limit, using defaults "
                      "without overwriting the file", sz, CONFIG_FILE_MAX);
        return ESP_OK;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json parse failed, using defaults without overwriting the file");
        return ESP_OK;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver) || ver->valueint != CFG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "unknown schema_version, using defaults without overwriting the file");
        cJSON_Delete(root);
        return ESP_OK;
    }

    load_from_json(&s_config, root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "config loaded: %u webradios, volume %d (max %d)",
             (unsigned)s_config.webradio_count, s_config.audio.volume, s_config.audio.volume_max);
    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LFS_BASE_PATH,
        .partition_label = LFS_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    s_cfg = heap_caps_calloc(1, sizeof(*s_cfg), MALLOC_CAP_SPIRAM);
    if (!s_cfg) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(mount_littlefs(), TAG, "littlefs init failed");
    ESP_RETURN_ON_ERROR(load_config(), TAG, "config load failed");
    s_ready = true;
    return ESP_OK;
}

const config_t *config_store_get(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "config_store_get before successful init");
    }
    return &s_config;
}

#define NVS_NAMESPACE   "brimbelle"
#define NVS_KEY_SSID    "wifi_ssid"   // slot 0; higher slots use wifi_ssid<N+1> (see wifi_keys)
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_PW_SALT "pw_salt"
#define NVS_KEY_PW_HASH "pw_hash"
#define NVS_KEY_MQTT_URL  "mqtt_url"
#define NVS_KEY_MQTT_USER "mqtt_user"
#define NVS_KEY_MQTT_PASS "mqtt_pass"
#define PW_SALT_LEN     16
#define PW_HASH_LEN     32

// Slot 0 is "wifi_ssid"/"wifi_pass"; slot N>0 is "wifi_ssid<N+1>"/"wifi_pass<N+1>".
// Keys stay within NVS's 15-char limit through slot 15 ("wifi_ssid16").
static void wifi_keys(int slot, char *ssid_key, char *pass_key, size_t sz)
{
    if (slot <= 0) {
        strlcpy(ssid_key, NVS_KEY_SSID, sz);
        strlcpy(pass_key, NVS_KEY_PASS, sz);
    } else {
        snprintf(ssid_key, sz, "wifi_ssid%d", slot + 1);
        snprintf(pass_key, sz, "wifi_pass%d", slot + 1);
    }
}

esp_err_t config_store_get_wifi_slot(int slot, char *ssid, size_t ssid_size,
                                     char *pass, size_t pass_size)
{
    char ssid_key[24], pass_key[24];
    wifi_keys(slot, ssid_key, pass_key, sizeof(ssid_key));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err; // ESP_ERR_NVS_NOT_FOUND if the namespace was never written
    }
    size_t len = ssid_size;
    err = nvs_get_str(h, ssid_key, ssid, &len);
    if (err == ESP_OK) {
        len = pass_size;
        err = nvs_get_str(h, pass_key, pass, &len);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_store_set_wifi_slot(int slot, const char *ssid, const char *pass)
{
    char ssid_key[24], pass_key[24];
    wifi_keys(slot, ssid_key, pass_key, sizeof(ssid_key));
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(h, ssid_key, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, pass_key, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void config_store_get_mqtt(config_mqtt_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(out->url);
    if (nvs_get_str(h, NVS_KEY_MQTT_URL, out->url, &len) != ESP_OK) out->url[0] = '\0';
    len = sizeof(out->user);
    if (nvs_get_str(h, NVS_KEY_MQTT_USER, out->user, &len) != ESP_OK) out->user[0] = '\0';
    len = sizeof(out->pass);
    if (nvs_get_str(h, NVS_KEY_MQTT_PASS, out->pass, &len) != ESP_OK) out->pass[0] = '\0';
    nvs_close(h);
}

esp_err_t config_store_set_mqtt(const config_mqtt_t *in)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(h, NVS_KEY_MQTT_URL, in->url);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_USER, in->user);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_PASS, in->pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_read_json(char *buf, size_t size, size_t *out_len)
{
    if (size == 0) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        if (s_lock) xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    if (s_lock) xSemaphoreGive(s_lock);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t config_store_write_json(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_ready || !s_lock) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver) || ver->valueint != CFG_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse into scratch memory and save THAT to disk first: if the save fails
    // (config over the size cap, full LittleFS), the live config must stay the
    // one on disk.
    config_t *scratch = heap_caps_calloc(1, sizeof(*scratch), MALLOC_CAP_SPIRAM);
    if (!scratch) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    set_defaults(scratch);
    load_from_json(scratch, root);
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = save_to_disk(scratch);
    if (err == ESP_OK) memcpy(s_cfg, scratch, sizeof(*scratch));
    free(scratch);
    xSemaphoreGive(s_lock);
    return err;
}

static bool hash_password(const uint8_t *salt, const char *plain, uint8_t *out)
{
    // SHA-256(salt || plain). psa_crypto_init is idempotent (IDF already ran
    // it at startup).
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t len = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&op, salt, PW_SALT_LEN) != PSA_SUCCESS ||
        psa_hash_update(&op, (const uint8_t *)plain, strlen(plain)) != PSA_SUCCESS ||
        psa_hash_finish(&op, out, PW_HASH_LEN, &len) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        ESP_LOGE(TAG, "password hash failed");
        return false;
    }
    return len == PW_HASH_LEN;
}

bool config_store_has_password(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PW_HASH, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len == PW_HASH_LEN;
}

esp_err_t config_store_set_password(const char *plain)
{
    uint8_t salt[PW_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[PW_HASH_LEN];
    if (!hash_password(salt, plain, hash)) return ESP_FAIL;

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_blob(h, NVS_KEY_PW_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_PW_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_clear_password(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t e1 = nvs_erase_key(h, NVS_KEY_PW_SALT);
    esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PW_HASH);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    // Already-absent keys count as cleared.
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) return e1;
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) return e2;
    return err;
}

esp_err_t config_store_check_password(const char *plain)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t salt[PW_SALT_LEN];
    uint8_t stored[PW_HASH_LEN];
    size_t sl = sizeof(salt), hl = sizeof(stored);
    esp_err_t e1 = nvs_get_blob(h, NVS_KEY_PW_SALT, salt, &sl);
    esp_err_t e2 = nvs_get_blob(h, NVS_KEY_PW_HASH, stored, &hl);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || sl != PW_SALT_LEN || hl != PW_HASH_LEN) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t calc[PW_HASH_LEN];
    if (!hash_password(salt, plain, calc)) return ESP_FAIL;  // reject, never "no password"
    // Constant-time compare to avoid leaking the hash via timing.
    uint8_t diff = 0;
    for (int i = 0; i < PW_HASH_LEN; i++) {
        diff |= calc[i] ^ stored[i];
    }
    return diff == 0 ? ESP_OK : ESP_FAIL;
}
