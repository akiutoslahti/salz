add_executable(salzcli salzcli.c)
set_target_properties(salzcli PROPERTIES OUTPUT_NAME "salz")
target_include_directories(salzcli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib")
target_link_libraries(salzcli PRIVATE salz)
