# audio owns I2S + amp enable + arbiter. audio_mix.c and audio_eq.c are pure C (host-tested).
idf_component_register(
    SRCS "audio.c" "audio_mix.c" "audio_eq.c" "audio_arbiter.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio board esp_timer
)
