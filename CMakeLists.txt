cmake_minimum_required(VERSION 3.10.0)
project (sds011 VERSION 1.0.0 LANGUAGES C)
set(CMAKE_C_STANDARD 23)

add_subdirectory(event_loop_static_mem)

add_executable(${CMAKE_PROJECT_NAME}
    main.c
    sds011.c
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE event_loop_static_mem
)
target_include_directories(${CMAKE_PROJECT_NAME}
    PRIVATE event_loop_static_mem/
)

