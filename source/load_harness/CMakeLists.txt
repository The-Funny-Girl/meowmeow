cmake_minimum_required(VERSION 3.20)

project(kirkware_load_harness LANGUAGES CXX)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The cooperative load harness requires Linux.")
endif()

include(CTest)

enable_testing()

option(KIRKWARE_LOAD_HARNESS_SANITIZERS "Enable ASan/UBSan" OFF)
option(KIRKWARE_LOAD_HARNESS_WERROR "Treat warnings as errors" ON)

function(kirkware_harness_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wformat=2
        -Wundef
    )

    if(KIRKWARE_LOAD_HARNESS_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    if(KIRKWARE_LOAD_HARNESS_SANITIZERS)
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

add_library(kirkware_load_test SHARED test_module.cpp)
set_target_properties(kirkware_load_test PROPERTIES
    OUTPUT_NAME kirkware_load_test
    CXX_VISIBILITY_PRESET hidden
)
kirkware_harness_target(kirkware_load_test)

add_executable(kirkware_load_target load_target.cpp)
set_target_properties(kirkware_load_target PROPERTIES
    OUTPUT_NAME kirkware-load-target
)
target_compile_definitions(kirkware_load_target PRIVATE _GNU_SOURCE=1)
target_link_libraries(kirkware_load_target PRIVATE ${CMAKE_DL_LIBS})
kirkware_harness_target(kirkware_load_target)

add_executable(kirkware_load_client load_client.cpp)
set_target_properties(kirkware_load_client PROPERTIES
    OUTPUT_NAME kirkware-load-client
)
kirkware_harness_target(kirkware_load_client)

add_test(
    NAME cooperative-load-self-test
    COMMAND kirkware_load_target --self-test $<TARGET_FILE:kirkware_load_test>
)
