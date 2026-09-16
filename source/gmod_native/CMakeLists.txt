cmake_minimum_required(VERSION 3.16)
project(kirkware_gmod_native LANGUAGES CXX)

set(GARRYSMOD_COMMON_DIR "" CACHE PATH "Path to garrysmod_common")
if(NOT GARRYSMOD_COMMON_DIR)
    message(FATAL_ERROR "GARRYSMOD_COMMON_DIR is required")
endif()

set(GMOD_INTERFACE "${GARRYSMOD_COMMON_DIR}/include/GarrysMod/Lua/Interface.h")
if(NOT EXISTS "${GMOD_INTERFACE}")
    message(FATAL_ERROR "garrysmod_common headers were not found at ${GARRYSMOD_COMMON_DIR}")
endif()

add_library(kirkware_native MODULE kirkware_native.cpp)
target_compile_features(kirkware_native PRIVATE cxx_std_17)
target_include_directories(kirkware_native PRIVATE "${GARRYSMOD_COMMON_DIR}/include")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kirkware_native PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
endif()

set_target_properties(kirkware_native PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    OUTPUT_NAME "gmcl_kirkware_native_linux64"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# GMod expects the Linux binary-module filename above even though the artifact is
# an ELF shared object. Also emit a conventional .so filename from the exact same
# compiled binary for standalone inspection and controlled in-process loading.
add_custom_command(TARGET kirkware_native POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:kirkware_native>"
            "${CMAKE_BINARY_DIR}/libkirkware_native.so"
    COMMENT "Creating conventional Linux shared-object artifact libkirkware_native.so"
)
