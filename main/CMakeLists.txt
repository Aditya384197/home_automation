idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        app_update
        driver
)
