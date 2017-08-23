
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")

add_library(mustache STATIC mustache.c mustache.h)
