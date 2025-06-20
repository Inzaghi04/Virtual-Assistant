file(GLOB_RECURSE I2S_AUDIO_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${I2S_AUDIO_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES driver
)