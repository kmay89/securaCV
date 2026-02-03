# SecuraCV OTA Component
#
# This component provides over-the-air firmware update capabilities
# for the SecuraCV Canary privacy witness device.

idf_component_register(
    SRCS "securacv_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp_https_ota
        app_update
        nvs_flash
        json
        mbedtls
        freertos
)

# Include Kconfig for menuconfig integration
set(COMPONENT_ADD_INCLUDEDIRS "include")
