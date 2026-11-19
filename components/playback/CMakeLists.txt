idf_component_register(
    SRCS "playback.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio config_store source_stream source_sendspin esp_timer
)
