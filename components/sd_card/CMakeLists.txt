file(GLOB_RECURSE SD_CARD_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${SD_CARD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc
)