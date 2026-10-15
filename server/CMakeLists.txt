cmake_minimum_required(VERSION 3.22.1)
project(stream_server C)

set(CMAKE_C_STANDARD 11)

add_library(stream_server SHARED
    stream_server.c
)

find_library(log-lib log)

target_link_libraries(stream_server
    PRIVATE
    ${log-lib}
)
