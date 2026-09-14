cmake_minimum_required(VERSION 3.20)

project(kirkware_linux VERSION 1.3.0 LANGUAGES CXX)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The Linux port requires a Linux host.")
endif()

include(GNUInstallDirs)
include(CTest)

option(KIRKWARE_BUILD_TESTS "Build Linux platform tests" ON)
option(KIRKWARE_ENABLE_SANITIZERS "Enable ASan/UBSan for Linux development" OFF)
option(KIRKWARE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

configure_file(
    source/platform/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/kirkware_version.hpp
    @ONLY
)

add_library(kirkware_platform STATIC
    source/platform/application.cpp
    source/platform/config_store.cpp
    source/platform/file_utils.cpp
    source/platform/game_integration.cpp
    source/platform/linux_paths.cpp
    source/platform/logger.cpp
    source/platform/workspace.cpp
)

target_include_directories(kirkware_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source/platform
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_features(kirkware_platform PUBLIC cxx_std_20)
target_compile_definitions(kirkware_platform PUBLIC KIRKWARE_PLATFORM_LINUX=1)

function(kirkware_configure_target target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wformat=2
        -Wundef
    )
    if(KIRKWARE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(KIRKWARE_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
            target_link_options(${target} PRIVATE
                -fsanitize=address,undefined
            )
        else()
            message(FATAL_ERROR "Sanitizers require GCC or Clang")
        endif()
    endif()
endfunction()

kirkware_configure_target(kirkware_platform)

add_executable(kirkware source/platform/linux_main.cpp)
target_link_libraries(kirkware PRIVATE kirkware_platform)
kirkware_configure_target(kirkware)

if(KIRKWARE_BUILD_TESTS AND BUILD_TESTING)
    add_executable(kirkware_platform_tests tests/linux_platform_tests.cpp)
    target_link_libraries(kirkware_platform_tests PRIVATE kirkware_platform)
    kirkware_configure_target(kirkware_platform_tests)

    add_test(NAME linux-platform COMMAND kirkware_platform_tests)
    add_test(NAME linux-cli-check COMMAND kirkware --check --no-workspace)
    add_test(NAME linux-cli-help COMMAND kirkware --help)
    add_test(NAME linux-cli-game-info COMMAND kirkware --game-info --no-workspace)
    add_test(NAME linux-cli-version COMMAND kirkware --version)
    add_test(NAME linux-cli-settings COMMAND kirkware --settings --no-workspace)
    add_test(NAME linux-cli-bad-option COMMAND kirkware --definitely-invalid)
    set_tests_properties(linux-cli-bad-option PROPERTIES WILL_FAIL TRUE)
endif()

install(TARGETS kirkware RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES BUILDING-LINUX.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "kirkware-linux")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "kirkware-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_NAME "kirkware-linux")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "The-Funny-Girl")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
