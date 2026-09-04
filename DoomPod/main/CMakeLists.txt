idf_component_register(
    SRCS "app_main.c" "i_video_esp.c" "i_system_esp.c" "i_sound_esp.c"
         "i_network_esp.c" "i_main_esp.c" "touch_input.c" "intro.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES prboom prboom-wad-tables esp_partition spi_flash driver esp_timer esp_lcd)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error -Wno-pointer-sign -Wno-implicit-function-declaration
    -Wno-unused-variable -Wno-format)
