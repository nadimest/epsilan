idf_component_register(
    SRCS "main.c" "sila_server.c" "cloud_config.c" "cloud_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_psram esp_wifi nvs_flash driver wifi_provisioning mdns nghttp esp-tls
    EMBED_TXTFILES "features/SiLAService.sila.xml" "features/Accelerometer.sila.xml" "features/CloudConfiguration.sila.xml"
)
