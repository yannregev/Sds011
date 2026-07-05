cmake_minimum_required(VERSION 3.15)
project(sds011 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)

add_subdirectory(src)
add_subdirectory(event_loop_static_mem)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()