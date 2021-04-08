cmake_minimum_required (VERSION 3.15.3)

project(salz C)

set(CMAKE_VERBOSE_MAKEFILE ON)

add_subdirectory(3rdparty)

add_compile_options(-O3 -march=native -Wall -Werror -Wextra -pedantic)

add_subdirectory(lib)
add_subdirectory(programs)
