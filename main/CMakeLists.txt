idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_woodfish.c"
         "woodfish_model.c"
         "woodfish_store.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_timer
)
