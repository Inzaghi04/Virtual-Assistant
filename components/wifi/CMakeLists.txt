file(GLOB_RECURSE WIFI_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${WIFI_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_system log mbedtls esp_http_client esp_http_server
)