add_executable(salzcli salzcli.c)

add_executable(bench_block bench_block.c)
target_include_directories(bench_block PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_BINARY_DIR}/../3rdparty/libdivsufsort/include")
target_link_libraries(bench_block PRIVATE salz divsufsort sais)
