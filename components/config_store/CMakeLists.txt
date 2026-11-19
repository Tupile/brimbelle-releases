idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash espressif__cjson joltwallet__littlefs
    PRIV_REQUIRES mbedtls esp_hw_support
)
