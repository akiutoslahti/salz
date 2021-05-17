cmake_minimum_required (VERSION 3.15.3)

project(salz C)

add_subdirectory(3rdparty)

if(CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_VERBOSE_MAKEFILE ON)
else()
    set(CMAKE_BUILD_TYPE "Release")
endif()

add_compile_options(-Wall -Werror -Wextra -pedantic)

option(ENABLE_STATS "Enable stat collection on compression" OFF)
if(ENABLE_STATS)
    add_definitions(-DENABLE_STATS)
endif()

add_subdirectory(lib)
add_subdirectory(programs)
