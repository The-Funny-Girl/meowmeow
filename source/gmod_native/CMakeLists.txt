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

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(KIRKWARE_CLIENT_LUA_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../gmod_addon/kirkware_linux/lua/autorun/client")
file(GLOB KIRKWARE_CLIENT_LUA CONFIGURE_DEPENDS
    "${KIRKWARE_CLIENT_LUA_DIR}/*.lua")
list(FILTER KIRKWARE_CLIENT_LUA EXCLUDE REGEX "/kirkware_linux_native\\.lua$")

set(KIRKWARE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(KIRKWARE_EMBEDDED_HEADER
    "${KIRKWARE_GENERATED_DIR}/kirkware_embedded_lua.hpp")

add_custom_command(
    OUTPUT "${KIRKWARE_EMBEDDED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KIRKWARE_GENERATED_DIR}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed_lua.py"
            --source "${KIRKWARE_CLIENT_LUA_DIR}"
            --output "${KIRKWARE_EMBEDDED_HEADER}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_lua.py"
        ${KIRKWARE_CLIENT_LUA}
    COMMENT "Embedding Kirkware client Lua feature payloads"
    VERBATIM
)

add_library(kirkware_native MODULE
    kirkware_native.cpp
    "${KIRKWARE_EMBEDDED_HEADER}"
)
target_compile_features(kirkware_native PRIVATE cxx_std_17)
target_include_directories(kirkware_native PRIVATE
    "${GARRYSMOD_COMMON_DIR}/include"
    "${KIRKWARE_GENERATED_DIR}"
)

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
# Append to source/gmod_native/CMakeLists.txt (reuses the embedded-Lua header
# target that is already defined above for kirkware_native).

add_library(kirkware_lua_loader SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/kirkware_lua_loader.cpp"
    "${KIRKWARE_EMBEDDED_HEADER}"
)
target_compile_features(kirkware_lua_loader PRIVATE cxx_std_17)
target_include_directories(kirkware_lua_loader PRIVATE
    "${GARRYSMOD_COMMON_DIR}/include"
    "${KIRKWARE_GENERATED_DIR}"
)
target_link_libraries(kirkware_lua_loader PRIVATE dl)
set_target_properties(kirkware_lua_loader PROPERTIES
    PREFIX "lib" OUTPUT_NAME "kirkware_lua_loader"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# For the inject variant instead of LD_PRELOAD:
#   target_compile_definitions(kirkware_lua_loader PRIVATE KIRKWARE_INJECT_MODE)
#   target_sources(kirkware_lua_loader PRIVATE /path/to/plthook/plthook_elf.c)
#   target_include_directories(kirkware_lua_loader PRIVATE /path/to/plthook)
