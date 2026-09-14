cmake_minimum_required(VERSION 3.20)

project(kirkware_linux VERSION 1.1.0 LANGUAGES CXX)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The Linux port requires a Linux host.")
endif()

include(GNUInstallDirs)
include(CTest)

option(KIRKWARE_BUILD_TESTS "Build Linux platform tests" ON)

add_library(kirkware_platform STATIC
    source/platform/application.cpp
    source/platform/file_utils.cpp
    source/platform/linux_paths.cpp
    source/platform/logger.cpp
    source/platform/workspace.cpp
)

target_include_directories(kirkware_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source/platform
)

target_compile_features(kirkware_platform PUBLIC cxx_std_20)
target_compile_definitions(kirkware_platform PUBLIC KIRKWARE_PLATFORM_LINUX=1)

function(kirkware_enable_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wformat=2
        -Wundef
    )
endfunction()

kirkware_enable_warnings(kirkware_platform)

add_executable(kirkware source/platform/linux_main.cpp)
target_link_libraries(kirkware PRIVATE kirkware_platform)
kirkware_enable_warnings(kirkware)

if(KIRKWARE_BUILD_TESTS AND BUILD_TESTING)
    add_executable(kirkware_platform_tests tests/linux_platform_tests.cpp)
    target_link_libraries(kirkware_platform_tests PRIVATE kirkware_platform)
    kirkware_enable_warnings(kirkware_platform_tests)

    add_test(NAME linux-platform COMMAND kirkware_platform_tests)
    add_test(NAME linux-cli-check COMMAND kirkware --check --no-workspace)
endif()

install(TARGETS kirkware RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES BUILDING-LINUX.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
