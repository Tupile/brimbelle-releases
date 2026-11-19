# encoder owns the KY-040 pins. buttons.c (copied from Bugne) and enc_logic.c
# are pure C (host-tested).
idf_component_register(
    SRCS "encoder.c" "enc_logic.c" "buttons.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_pcnt esp_driver_gpio board audio playback source_sendspin ha_mqtt
)
