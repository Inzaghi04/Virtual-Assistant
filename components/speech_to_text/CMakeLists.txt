file(GLOB_RECURSE SPEECH_TO_TEXT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${SPEECH_TO_TEXT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client mbedtls freertos log json driver sdmmc fatfs esp_timer
)