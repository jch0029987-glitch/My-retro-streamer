cmake_minimum_required(VERSION 3.22.1)

# Initialize project with C and let the NDK toolchain manage the compiler cleanly
project(stream_server VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(stream_server SHARED
    stream_server.c
)

find_library(log-lib log)

target_link_libraries(stream_server
    PRIVATE
    ${log-lib}
)
