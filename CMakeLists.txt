cmake_minimum_required (VERSION 3.15.3)

project(salz C)

add_subdirectory(3rdparty)

if(CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_VERBOSE_MAKEFILE ON)
endif()

add_compile_options(-O3 -Wall -Werror -Wextra -pedantic)

add_subdirectory(lib)
add_subdirectory(programs)
