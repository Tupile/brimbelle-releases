# The app entry point. REQUIRES lists the components the boot sequence calls
# directly. Each component pulls its own further dependencies.
idf_component_register(
    SRCS "brimbelle_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        esp_pm
        logstore
        config_store
        board
        audio
        net
        web_config
        source_stream
        source_sendspin
        playback
        encoder
        ha_mqtt
)
