cmake_minimum_required(VERSION 3.25)

project(McastPilot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
)

add_library(mcast_core STATIC
    src/common/log.cpp
    src/common/byte_ring.cpp
    src/net/nic.cpp
    src/net/platform.cpp
    src/net/multicast_sender.cpp
    src/media/pipeline.cpp
)

target_include_directories(mcast_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcast_core PUBLIC PkgConfig::FFMPEG)

if(WIN32)
    target_compile_definitions(mcast_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(mcast_core PUBLIC ws2_32 iphlpapi winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mcast_core PUBLIC Threads::Threads)
endif()

add_executable(mcastctl src/cli/main.cpp)
target_link_libraries(mcastctl PRIVATE mcast_core)
