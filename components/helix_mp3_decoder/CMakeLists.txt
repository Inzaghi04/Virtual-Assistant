file(GLOB_RECURSE HELIX_MP3_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${HELIX_MP3_SRCS}
    INCLUDE_DIRS "include"
)