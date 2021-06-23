add_executable(salzcli salzcli.c)
set_target_properties(salzcli PROPERTIES OUTPUT_NAME "salz")
target_include_directories(salzcli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib")
target_link_libraries(salzcli PRIVATE salz)

add_executable(bench_block bench_block.c)
target_include_directories(bench_block PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_BINARY_DIR}/../3rdparty/libdivsufsort/include")
target_link_libraries(bench_block PRIVATE divsufsort sais)
