cmake_minimum_required(VERSION 3.25)

project(McastPilot VERSION 0.3.0 LANGUAGES CXX)

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
    libswscale
)

add_library(mcast_core STATIC
    src/common/log.cpp
    src/common/byte_ring.cpp
    src/net/nic.cpp
    src/net/platform.cpp
    src/net/multicast_sender.cpp
    src/net/udp_receiver.cpp
    src/media/pipeline.cpp
    src/media/probe.cpp
    src/media/transcoder.cpp
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

add_executable(mcastd
    src/server/main.cpp
    src/common/config.cpp
    src/common/file_log.cpp
    src/task/task.cpp
    src/task/task_manager.cpp
    src/api/http_server.cpp
)
target_include_directories(mcastd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(mcastd PRIVATE mcast_core)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/web")
    add_custom_target(copy_web ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:mcastd>/web"
        DEPENDS mcastd
        COMMENT "Copying web assets next to mcastd")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(mcastgui
    src/gui/main.cpp
    src/gui/api_client.cpp
    src/gui/main_window.cpp
    src/gui/task_dialog.cpp
    src/gui/dialogs.cpp
)
target_include_directories(mcastgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcastgui PRIVATE Qt6::Widgets Qt6::Network)

if(WIN32)
    set_target_properties(mcastgui PROPERTIES WIN32_EXECUTABLE TRUE)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt)
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET mcastgui POST_BUILD
            COMMAND ${WINDEPLOYQT_EXECUTABLE} --release --no-translations
                "$<TARGET_FILE:mcastgui>"
            COMMENT "Deploying Qt runtime next to mcastgui")
    else()
        message(WARNING "windeployqt not found; use scripts/mcastgui.ps1 to run the GUI")
    endif()

    # windeployqt in this MSYS2 toolchain does not always pick up non-Qt
    # dependencies such as libfreetype; copy the known runtime set explicitly.
    get_target_property(QT_CORE_DLL Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT QT_CORE_DLL)
        get_target_property(QT_CORE_DLL Qt6::Core IMPORTED_LOCATION)
    endif()
    if(QT_CORE_DLL)
        get_filename_component(QT_RUNTIME_BIN "${QT_CORE_DLL}" DIRECTORY)
        set(GUI_RUNTIME_DLLS
            libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll
            libbrotlidec.dll libbrotlicommon.dll zlib1.dll libzstd.dll
            libfreetype-6.dll libharfbuzz-0.dll libpng16-16.dll
            libb2-1.dll libdouble-conversion.dll
            libicuin78.dll libicuuc78.dll libicudt78.dll
            libpcre2-16-0.dll libpcre2-8-0.dll libmd4c.dll
            libbz2-1.dll libgraphite2.dll libglib-2.0-0.dll
            libintl-8.dll libiconv-2.dll
        )
        foreach(GUI_DLL IN LISTS GUI_RUNTIME_DLLS)
            if(EXISTS "${QT_RUNTIME_BIN}/${GUI_DLL}")
                add_custom_command(TARGET mcastgui POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QT_RUNTIME_BIN}/${GUI_DLL}"
                        "$<TARGET_FILE_DIR:mcastgui>/${GUI_DLL}")
            endif()
        endforeach()
    endif()
endif()
