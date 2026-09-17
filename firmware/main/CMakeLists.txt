idf_component_register(
    SRCS "main.c" "sila_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_psram esp_wifi nvs_flash driver wifi_provisioning mdns nghttp
    EMBED_TXTFILES "features/SiLAService.sila.xml" "features/Accelerometer.sila.xml"
)
