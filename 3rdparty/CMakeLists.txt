add_library(sais)
target_sources(sais PRIVATE libsais/src/libsais.c)
target_include_directories(sais PUBLIC libsais/src)
target_compile_options(sais PRIVATE -Wall -O3)
