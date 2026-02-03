# Main application component

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        securacv_ota
        wifi_sta
        nvs_flash
        esp_system
        esp_ota_ops
        freertos
)
