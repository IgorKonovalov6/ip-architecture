cmake_minimum_required(VERSION 3.16)

project(udp_exchange VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT UNIX)
    message(WARNING "udp_node предназначен для компиляции и запуска в Linux")
endif()

add_executable(udp_node src/main.cpp)
target_include_directories(udp_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(udp_node PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()

install(TARGETS udp_node RUNTIME DESTINATION bin)
