cmake_minimum_required(VERSION 3.10.0)

add_library(sds011_lib STATIC
    sds011.c
)

target_include_directories(sds011_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/event_loop_static_mem/   
    ${CMAKE_SOURCE_DIR}/include
)

add_executable(${CMAKE_PROJECT_NAME}
    main.c
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE 
    event_loop_static_mem
    sds011_lib
)
