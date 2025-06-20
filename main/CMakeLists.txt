idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES helix_mp3_decoder driver esp_system esp_event fatfs sdmmc nvs_flash log esp_http_client json 
                                            esp_netif mbedtls esp_wifi esp_timer esp_http_server i2s_audio 
                                            sd_card wifi audio
            
)