#pragma once

#include <cstddef>
#include <cstdint>

struct KirkwareDeferredEventContext {
    std::uint64_t sleep_function = 0;
    std::uint64_t read_process_memory = 0;
    std::uint64_t write_process_memory = 0;
    std::uint64_t virtual_protect = 0;
    std::uint64_t installer_function = 0;
    std::uint64_t image_base = 0;
    std::uint64_t event_object = 0;
    std::uint64_t event_vtable = 0;
    std::uint64_t event_entry_slot = 0;
    std::uint64_t event_original = 0;
    std::uint64_t event_detour = 0;
    std::uint64_t event_saved_slot = 0;
    std::uint64_t lua_detour = 0;
    std::uint64_t lua_vtable_a = 0;
    std::uint64_t lua_vtable_b = 0;
    std::uint64_t lua_original = 0;
    std::uint64_t lua_original_prefix_a = 0;
    std::uint64_t lua_original_prefix_b = 0;
    std::uint64_t lua_detour_prefix_a = 0;
    std::uint64_t lua_detour_prefix_b = 0;
    std::uint64_t event_vector_slots = 0;
    std::uint64_t expected_vector_begin = 0;
    std::uint64_t expected_vector_end = 0;
    std::uint64_t expected_vector_capacity = 0;
    std::uint64_t expected_records_address = 0;
    std::uint64_t current_records_address = 0;
    std::uint64_t expected_records_bytes = 0;
    std::uint64_t current_records_capacity = 0;
    std::uint64_t close_handle = 0;
    std::uint64_t mutex_handle = 0;
    std::uint32_t armed = 0;
    std::uint32_t arm_attempts = 0;
    volatile std::uint32_t acknowledged = 0;
    std::uint32_t status = 0;
    std::uint32_t attempts = 0;
    std::uint32_t reserved = 0;
    std::uint64_t result = UINT64_MAX;
};

static_assert(sizeof(KirkwareDeferredEventContext) == 272);
static_assert(offsetof(KirkwareDeferredEventContext, armed) == 240);
static_assert(offsetof(KirkwareDeferredEventContext, acknowledged) == 248);
static_assert(offsetof(KirkwareDeferredEventContext, status) == 252);
static_assert(offsetof(KirkwareDeferredEventContext, result) == 264);
