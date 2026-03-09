cmake_minimum_required(VERSION 3.10.0)
project(dialdown VERSION 0.1.0 LANGUAGES C)

add_executable(dialdown main.c)

add_subdirectory(kissfft)
target_include_directories(dialdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kissfft)

target_link_libraries(dialdown
    asound
    m
    kissfft
)