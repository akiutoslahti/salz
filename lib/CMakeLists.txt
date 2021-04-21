add_library(salz)
file(GLOB SOURCES "*.c")
target_sources(salz PRIVATE ${SOURCES})
target_include_directories(salz PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_BINARY_DIR}/../3rdparty/libdivsufsort/include")
target_link_libraries(salz PRIVATE divsufsort)
