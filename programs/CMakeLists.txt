add_executable(salzcli salzcli.c)
target_include_directories(salzcli PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(salzcli PRIVATE salz)
