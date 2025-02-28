cmake_minimum_required(VERSION 3.10)

project(obs-nvfbc
    VERSION 1.0.1
    DESCRIPTION "obs-nvfbc: NvFBC support for obs-studio v30+"
    LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB SOURCES "src/*.c")

include_directories(include)

add_library(obs-nvfbc SHARED ${SOURCES})
target_link_libraries(obs-nvfbc nvidia-fbc dl vulkan obs EGL)

add_library(obs-nvfbc-pre MODULE src/hooks/hooks.c)
target_link_libraries(obs-nvfbc-pre dl)

target_compile_options(obs-nvfbc PRIVATE
    -Wall -Wextra
    -fPIC
)
