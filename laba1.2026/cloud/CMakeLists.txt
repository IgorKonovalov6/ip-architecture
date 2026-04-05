cmake_minimum_required(VERSION 3.10)
project(CloudDetector)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(cloud
    src/main.cpp
    src/motor.cpp
)

target_include_directories(cloud PRIVATE src)