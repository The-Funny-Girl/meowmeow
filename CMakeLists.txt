cmake_minimum_required(VERSION 3.20)

project(kirkware_linux VERSION 1.0 LANGUAGES CXX)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The Linux port requires a Linux host.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(kirkware
    source/platform/linux_main.cpp
)

target_compile_options(kirkware PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wformat=2
    -Wundef
)

target_compile_definitions(kirkware PRIVATE
    KIRKWARE_PLATFORM_LINUX=1
)

include(GNUInstallDirs)
install(TARGETS kirkware RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
