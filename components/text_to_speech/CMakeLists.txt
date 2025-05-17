file(GLOB_RECURSE TEXT_TO_SPEECH_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${TEXT_TO_SPEECH_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client mbedtls freertos log json
)