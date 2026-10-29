idf_component_register(SRCS "esp32s3_gateway.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_wifi esp_event esp_netif)