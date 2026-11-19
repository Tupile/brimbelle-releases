idf_component_register(
    SRCS "ha_mqtt.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__mqtt espressif__cjson config_store board net esp_app_format playback decode
)
