#include "kirkware_deferred_event_worker.hpp"

namespace {

using SleepFunction = void (*)(std::uint32_t);
using ReadProcessMemoryFunction = int (*)(void*, const void*, void*,
                                          std::size_t, std::size_t*);
using WriteProcessMemoryFunction = int (*)(void*, void*, const void*,
                                           std::size_t, std::size_t*);
using VirtualProtectFunction = int (*)(void*, std::size_t, std::uint32_t,
                                       std::uint32_t*);
using CloseHandleFunction = int (*)(void*);
using InstallerFunction = std::uint64_t (*)(std::uint64_t, std::uint64_t,
                                            std::uint64_t, std::uint64_t);

__attribute__((always_inline)) inline void* CurrentProcessHandle() {
    return reinterpret_cast<void*>(~static_cast<std::uintptr_t>(0));
}

__attribute__((always_inline)) inline bool ReadMemory(
    KirkwareDeferredEventContext* context, std::uint64_t address,
    void* output, std::size_t size) {
    const auto function = reinterpret_cast<ReadProcessMemoryFunction>(
        context->read_process_memory);
    return function(CurrentProcessHandle(),
                    reinterpret_cast<const void*>(address), output, size,
                    nullptr) != 0;
}

__attribute__((always_inline)) inline bool ReadQword(
    KirkwareDeferredEventContext* context, std::uint64_t address,
    std::uint64_t& output) {
    return ReadMemory(context, address, &output, sizeof(output));
}

__attribute__((always_inline)) inline bool WriteQword(
    KirkwareDeferredEventContext* context, std::uint64_t address,
    std::uint64_t value) {
    const auto function = reinterpret_cast<WriteProcessMemoryFunction>(
        context->write_process_memory);
    return function(CurrentProcessHandle(), reinterpret_cast<void*>(address),
                    &value, sizeof(value), nullptr) != 0;
}

__attribute__((always_inline)) inline bool WriteProtectedQword(
    KirkwareDeferredEventContext* context, std::uint64_t address,
    std::uint64_t value) {
    const auto protect = reinterpret_cast<VirtualProtectFunction>(
        context->virtual_protect);
    std::uint32_t previous = 0;
    if (!protect(reinterpret_cast<void*>(address), sizeof(value), 0x04,
                 &previous))
        return false;
    const bool wrote = WriteQword(context, address, value);
    std::uint32_t ignored = 0;
    const bool restored =
        protect(reinterpret_cast<void*>(address), sizeof(value), previous,
                &ignored) != 0;
    std::uint64_t observed = 0;
    return wrote && restored && ReadQword(context, address, observed) &&
           observed == value;
}

__attribute__((always_inline)) inline bool EqualBytes(
    std::uint64_t left_address, std::uint64_t right_address,
    std::size_t size) {
    const auto* left = reinterpret_cast<const volatile std::uint8_t*>(
        left_address);
    const auto* right = reinterpret_cast<const volatile std::uint8_t*>(
        right_address);
    for (std::size_t index = 0; index != size; ++index) {
        if (left[index] != right[index])
            return false;
    }
    return true;
}

__attribute__((always_inline)) inline std::uint64_t LocalQword(
    std::uint64_t address) {
    const auto* bytes = reinterpret_cast<const volatile std::uint8_t*>(address);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    return value;
}

__attribute__((always_inline)) inline std::uint32_t LocalDword(
    std::uint64_t address) {
    const auto* bytes = reinterpret_cast<const volatile std::uint8_t*>(address);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index != 4; ++index)
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    return value;
}

__attribute__((always_inline)) inline bool ReadVector(
    KirkwareDeferredEventContext* context, std::uint64_t& begin,
    std::uint64_t& end, std::uint64_t& capacity) {
    return ReadQword(context, context->event_vector_slots, begin) &&
           ReadQword(context, context->event_vector_slots + 8, end) &&
           ReadQword(context, context->event_vector_slots + 16, capacity);
}

__attribute__((always_inline)) inline bool ReadinessExact(
    KirkwareDeferredEventContext* context) {
    std::uint64_t realm = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry = 0;
    std::uint64_t lua_state = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    std::uint64_t registered = 0;
    std::uint64_t value = 0;
    std::uint64_t prefixes[2]{};
    if (!ReadQword(context, context->image_base + 0x7EA870, realm) ||
        !realm || !ReadQword(context, realm, vtable) ||
        (vtable != context->lua_vtable_a &&
         vtable != context->lua_vtable_b) ||
        !ReadQword(context, vtable + 111 * 8, entry) ||
        entry != context->lua_detour ||
        !ReadQword(context, realm + 8, lua_state) || !lua_state ||
        !ReadQword(context, context->image_base + 0xB6B338, begin) ||
        !ReadQword(context, context->image_base + 0xB6B340, end) ||
        !ReadQword(context, context->image_base + 0xB6B348, capacity) ||
        !begin || end != begin + 8 || capacity != end ||
        !ReadQword(context, begin, registered) || registered != lua_state)
        return false;
    constexpr std::uint32_t ready_rvas[]{0xB69478, 0xB694F8, 0xB69508};
    for (const std::uint32_t rva : ready_rvas) {
        if (!ReadQword(context, context->image_base + rva, value) || value != 1)
            return false;
    }
    if (!ReadQword(context, context->image_base + 0x823868, value) ||
        value != context->lua_original ||
        !ReadMemory(context, context->lua_original, prefixes,
                    sizeof(prefixes)) ||
        prefixes[0] != context->lua_original_prefix_a ||
        prefixes[1] != context->lua_original_prefix_b ||
        !ReadMemory(context, context->lua_detour, prefixes,
                    sizeof(prefixes)) ||
        prefixes[0] != context->lua_detour_prefix_a ||
        prefixes[1] != context->lua_detour_prefix_b ||
        !ReadQword(context, context->image_base + 0x7EA870, value) ||
        value != realm)
        return false;
    return true;
}

__attribute__((always_inline)) inline bool ExpectedRecordsExact(
    KirkwareDeferredEventContext* context, std::uint64_t begin,
    std::uint64_t bytes) {
    if (bytes != context->expected_records_bytes ||
        bytes > context->current_records_capacity)
        return false;
    if (!bytes)
        return true;
    return context->expected_records_address &&
           context->current_records_address &&
           ReadMemory(context, begin,
                      reinterpret_cast<void*>(context->current_records_address),
                      static_cast<std::size_t>(bytes)) &&
           EqualBytes(context->expected_records_address,
                      context->current_records_address,
                      static_cast<std::size_t>(bytes));
}

__attribute__((always_inline)) inline bool EventPreExact(
    KirkwareDeferredEventContext* context) {
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry = 0;
    std::uint64_t saved = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    return ReadQword(context, context->image_base + 0x7EA840, object) &&
           object == context->event_object &&
           ReadQword(context, object, vtable) &&
           vtable == context->event_vtable &&
           ReadQword(context, context->event_entry_slot, entry) &&
           entry == context->event_original &&
           ReadQword(context, context->event_saved_slot, saved) &&
           saved == context->event_original &&
           ReadVector(context, begin, end, capacity) &&
           begin == context->expected_vector_begin &&
           end == context->expected_vector_end &&
           capacity == context->expected_vector_capacity &&
           ExpectedRecordsExact(context, begin,
                                context->expected_records_bytes);
}

__attribute__((always_inline)) inline bool EventPostExact(
    KirkwareDeferredEventContext* context) {
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry = 0;
    std::uint64_t saved = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    if (!ReadQword(context, context->image_base + 0x7EA840, object) ||
        object != context->event_object || !ReadQword(context, object, vtable) ||
        vtable != context->event_vtable ||
        !ReadQword(context, vtable + 8 * 8, entry) ||
        entry != context->event_detour ||
        !ReadQword(context, context->event_saved_slot, saved) ||
        saved != context->event_original ||
        !ReadVector(context, begin, end, capacity) || !begin || end < begin ||
        capacity < end || end - begin != context->expected_records_bytes + 24 ||
        capacity - begin > 4096 * 24 || (capacity - begin) % 24 != 0 ||
        end - begin > context->current_records_capacity ||
        !ReadMemory(context, begin,
                    reinterpret_cast<void*>(context->current_records_address),
                    static_cast<std::size_t>(end - begin)) ||
        !EqualBytes(context->expected_records_address,
                    context->current_records_address,
                    static_cast<std::size_t>(context->expected_records_bytes)))
        return false;
    const std::uint64_t appended =
        context->current_records_address + context->expected_records_bytes;
    return LocalQword(appended) == context->event_object &&
           LocalQword(appended + 8) == context->event_original &&
           LocalDword(appended + 16) == 8;
}

__attribute__((always_inline)) inline bool RollbackEvent(
    KirkwareDeferredEventContext* context) {
    bool detached = false;
    std::uint64_t entry = 0;
    if (ReadQword(context, context->event_entry_slot, entry)) {
        if (entry == context->event_original)
            detached = true;
        else if (entry == context->event_detour)
            detached = WriteProtectedQword(
                context, context->event_entry_slot, context->event_original);
    }

    bool records_restored = false;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    if (ReadVector(context, begin, end, capacity)) {
        if (begin == context->expected_vector_begin &&
            end == context->expected_vector_end &&
            capacity == context->expected_vector_capacity &&
            ExpectedRecordsExact(context, begin,
                                 context->expected_records_bytes)) {
            records_restored = true;
        } else if (begin && end >= begin && capacity >= end &&
                   end - begin == context->expected_records_bytes + 24 &&
                   capacity - begin <= 4096 * 24 &&
                   (capacity - begin) % 24 == 0 &&
                   end - begin <= context->current_records_capacity &&
                   ReadMemory(context, begin,
                              reinterpret_cast<void*>(
                                  context->current_records_address),
                              static_cast<std::size_t>(end - begin)) &&
                   EqualBytes(context->expected_records_address,
                              context->current_records_address,
                              static_cast<std::size_t>(
                                  context->expected_records_bytes))) {
            const std::uint64_t appended =
                context->current_records_address +
                context->expected_records_bytes;
            if (LocalQword(appended) == context->event_object &&
                LocalQword(appended + 8) == context->event_original &&
                LocalDword(appended + 16) == 8) {
                records_restored = WriteQword(
                    context, context->event_vector_slots + 8, end - 24);
            }
        }
    }
    return detached && records_restored &&
           WriteQword(context, context->event_saved_slot, 0);
}

__attribute__((always_inline)) inline std::uint32_t Finish(
    KirkwareDeferredEventContext* context, std::uint32_t status) {
    context->status = status;
    if (context->mutex_handle && context->close_handle) {
        const auto close =
            reinterpret_cast<CloseHandleFunction>(context->close_handle);
        if (close(reinterpret_cast<void*>(context->mutex_handle)))
            context->mutex_handle = 0;
        else
            context->status = 11;
    }
    return 0;
}

}

extern "C" __attribute__((section(".kw_evt$m"), noinline, used))
std::uint32_t KirkwareDeferredEventWorker(
    KirkwareDeferredEventContext* context) {
    if (!context || !context->sleep_function ||
        !context->read_process_memory || !context->write_process_memory ||
        !context->virtual_protect || !context->installer_function ||
        !context->close_handle) {
        return 0;
    }
    const auto sleep =
        reinterpret_cast<SleepFunction>(context->sleep_function);
    std::uint32_t armed = 0;
    while (context->arm_attempts != 400) {
        if (!ReadMemory(context,
                        reinterpret_cast<std::uint64_t>(&context->armed),
                        &armed, sizeof(armed)) || armed > 1)
            return Finish(context, 9);
        if (armed == 1)
            break;
        ++context->arm_attempts;
        sleep(25);
    }
    if (armed != 1)
        return Finish(context, 8);
    std::uint64_t saved = 0;
    if (!ReadQword(context, context->event_saved_slot, saved) ||
        (saved != 0 && saved != context->event_original) ||
        (saved == 0 &&
         !WriteProtectedQword(context, context->event_saved_slot,
                              context->event_original)))
        return Finish(context, 3);
    context->acknowledged = 1;
    for (;;) {
        ++context->attempts;
        if (ReadinessExact(context)) {
            if (EventPostExact(context)) {
                context->result = context->event_original;
                return Finish(context, 1);
            }
            if (EventPreExact(context))
                break;
        }
        sleep(25);
    }

    const auto installer =
        reinterpret_cast<InstallerFunction>(context->installer_function);
    context->result = installer(0, context->event_detour, 8, 0);
    if (context->result != context->event_original) {
        return Finish(context, RollbackEvent(context) ? 5 : 7);
    }
    if (!EventPostExact(context)) {
        return Finish(context, RollbackEvent(context) ? 6 : 7);
    }
    return Finish(context, 1);
}
