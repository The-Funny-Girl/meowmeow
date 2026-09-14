#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef KIRKWARE_TRIGGER_STACK_SIZE
#define KIRKWARE_TRIGGER_STACK_SIZE 0
#endif

namespace {

constexpr std::uint32_t kTlsDataRva = 0x696D40;
constexpr std::size_t kTlsDataSize = 0x14C;
constexpr std::uint32_t kTlsIndexRva = 0x7E4790;
constexpr std::uint32_t kSourceGuardRva = 0xB6ECDC;
constexpr std::uint32_t kSentinelSlotRva = 0xB6E978;
constexpr std::uint32_t kResolvedTimeSlotRva = 0x7E5130;
constexpr std::uint32_t kResolvedTimeStoreRva = 0x4A6150;
constexpr std::size_t kHandlerTriggerPcOffset = 0x0D;
constexpr std::size_t kHandlerImageBaseOffset = 0x2F;
constexpr std::size_t kHandlerEntryOffset = 0x40;
constexpr std::size_t kHandlerSize = 0x63;
#ifdef KIRKWARE_EXTERNAL_STATIC_TLS_CARRIER
constexpr std::uint64_t kExternalTlsCarrierBase = 0x1E5DF940000ull;
#endif

struct TriggerContext {
    std::uint64_t tls_index;
    std::uint64_t tls_block;
    std::uint64_t tls_set_value;
    std::uint64_t image_base;
    std::uint64_t callbacks[3];
    std::uint64_t entry_argument;
    std::uint64_t status;
    std::uint64_t tls_slot_after_set;
    std::uint64_t tls_slot_after_callbacks;
    std::uint64_t tls_flag_before_reset;
    std::uint64_t tls_flag_after_reset;
    std::uint64_t crt_init_result;
    std::uint64_t crt_ptd_result;
    std::uint64_t stack_base;
    std::uint64_t stack_limit;
    std::uint64_t deallocation_stack;
};

struct SentinelWatcherContext {
    std::uint64_t slot_address;
    std::uint64_t status;
    std::uint64_t observed_node;
};

struct ReturnCaptureState {
    std::uint64_t marker;
    std::uint64_t release;
    std::uint64_t context_address;
};

static_assert(offsetof(TriggerContext, tls_block) == 0x08);
static_assert(offsetof(TriggerContext, tls_set_value) == 0x10);
static_assert(offsetof(TriggerContext, image_base) == 0x18);
static_assert(offsetof(TriggerContext, callbacks) == 0x20);
static_assert(offsetof(TriggerContext, entry_argument) == 0x38);
static_assert(offsetof(TriggerContext, status) == 0x40);
static_assert(offsetof(TriggerContext, tls_slot_after_set) == 0x48);
static_assert(offsetof(TriggerContext, tls_slot_after_callbacks) == 0x50);
static_assert(offsetof(TriggerContext, tls_flag_before_reset) == 0x58);
static_assert(offsetof(TriggerContext, tls_flag_after_reset) == 0x60);
static_assert(offsetof(TriggerContext, crt_init_result) == 0x68);
static_assert(offsetof(TriggerContext, crt_ptd_result) == 0x70);
static_assert(offsetof(TriggerContext, stack_base) == 0x78);
static_assert(offsetof(TriggerContext, stack_limit) == 0x80);
static_assert(offsetof(TriggerContext, deallocation_stack) == 0x88);
static_assert(offsetof(SentinelWatcherContext, status) == 0x08);
static_assert(offsetof(SentinelWatcherContext, observed_node) == 0x10);
static_assert(offsetof(ReturnCaptureState, release) == 0x08);
static_assert(offsetof(ReturnCaptureState, context_address) == 0x10);

std::vector<std::uint8_t> BuildTriggerStub() {
    std::vector<std::uint8_t> code;
    const auto emit = [&code](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    emit({0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9});
#ifdef KIRKWARE_EXPAND_STATIC_TLS_VECTOR
    emit({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
          0x48, 0x89, 0x43, 0x48,
          0x48, 0xC7, 0x43, 0x40, 4, 0, 0, 0,
          0x48, 0x83, 0x7B, 0x40, 5, 0x74, 4,
          0xF3, 0x90, 0xEB, 0xF5,
          0x48, 0x8B, 0x43, 0x50,
          0x65, 0x48, 0x89, 0x04, 0x25, 0x58, 0, 0, 0,
          0x8B, 0x0B, 0x48, 0x8B, 0x53, 0x08,
          0x48, 0x89, 0x14, 0xC8});
#else
    emit({0x8B, 0x0B, 0x48, 0x8B, 0x53, 0x08, 0xFF, 0x53, 0x10});
    emit({0x85, 0xC0, 0x0F, 0x84});
    const std::size_t failure_displacement = code.size();
    emit({0, 0, 0, 0});
#endif
    emit({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
          0x8B, 0x0B, 0x48, 0x8B, 0x04, 0xC8,
          0x48, 0x89, 0x43, 0x48});
    emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
          0x45, 0x33, 0xC0, 0xFF, 0x53, 0x20});
    emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
          0x45, 0x33, 0xC0, 0xFF, 0x53, 0x28});
    emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
          0x45, 0x33, 0xC0, 0xFF, 0x53, 0x30});
#ifdef KIRKWARE_CRT_THREAD_INIT
    emit({0x31, 0xC9, 0x48, 0xB8});
    const std::uint64_t crt_initializer = 0x1E5DD0BE51Cull;
    const auto* crt_initializer_bytes =
        reinterpret_cast<const std::uint8_t*>(&crt_initializer);
    code.insert(code.end(), crt_initializer_bytes,
                crt_initializer_bytes + sizeof(crt_initializer));
    emit({0xFF, 0xD0, 0x48, 0x89, 0x43, 0x68,
          0x48, 0xB8});
    const std::uint64_t crt_get_ptd = 0x1E5DD0BE400ull;
    const auto* crt_get_ptd_bytes =
        reinterpret_cast<const std::uint8_t*>(&crt_get_ptd);
    code.insert(code.end(), crt_get_ptd_bytes,
                crt_get_ptd_bytes + sizeof(crt_get_ptd));
    emit({0xFF, 0xD0, 0x48, 0x89, 0x43, 0x70});
#endif
    emit({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
          0x8B, 0x0B, 0x48, 0x8B, 0x04, 0xC8,
          0x48, 0x89, 0x43, 0x50,
          0x48, 0xC7, 0x43, 0x40, 2, 0, 0, 0,
          0x48, 0x83, 0x7B, 0x40, 3, 0x74, 4,
          0xF3, 0x90, 0xEB, 0xF5,
          0x48, 0xC7, 0x43, 0x40, 1, 0, 0, 0,
          0x48, 0x8B, 0x53, 0x38,
          0x48, 0x83, 0xC4, 0x20, 0x5B,
          0x0F, 0x20, 0xC0, 0x33, 0xC0, 0xC3});
#ifndef KIRKWARE_EXPAND_STATIC_TLS_VECTOR
    const std::size_t failure = code.size();
    emit({0x48, 0xC7, 0x43, 0x40, 0xFF, 0xFF, 0xFF, 0xFF,
          0x48, 0x83, 0xC4, 0x20, 0x5B, 0x33, 0xC0, 0xC3});
    const auto relative = static_cast<std::int32_t>(
        failure - (failure_displacement + sizeof(std::int32_t)));
    std::memcpy(code.data() + failure_displacement, &relative,
                sizeof(relative));
#endif
    return code;
}

std::vector<std::uint8_t> BuildLoaderManagedTriggerStub(
    std::uint64_t sleep_address,
    bool run_process_attach_callbacks = false) {
    std::vector<std::uint8_t> code;
    const auto emit = [&code](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    emit({0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9});
#ifdef KIRKWARE_CRT_THREAD_INIT
    emit({0x31, 0xC9, 0x48, 0xB8});
    const std::uint64_t crt_initializer = 0x1E5DD0BE51Cull;
    const auto* crt_initializer_bytes =
        reinterpret_cast<const std::uint8_t*>(&crt_initializer);
    code.insert(code.end(), crt_initializer_bytes,
                crt_initializer_bytes + sizeof(crt_initializer));
    emit({0xFF, 0xD0, 0x48, 0x89, 0x43, 0x68,
          0x48, 0xB8});
    const std::uint64_t crt_get_ptd = 0x1E5DD0BE400ull;
    const auto* crt_get_ptd_bytes =
        reinterpret_cast<const std::uint8_t*>(&crt_get_ptd);
    code.insert(code.end(), crt_get_ptd_bytes,
                crt_get_ptd_bytes + sizeof(crt_get_ptd));
    emit({0xFF, 0xD0, 0x48, 0x89, 0x43, 0x70});
#endif
    emit({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
          0x8B, 0x0B, 0x48, 0x8B, 0x04, 0xC8,
          0x48, 0x89, 0x43, 0x48});
#ifdef KIRKWARE_EXTERNAL_STATIC_TLS_CARRIER
    emit({0x0F, 0xB6, 0x48, 0x14,
          0x48, 0x89, 0x4B, 0x58,
          0xC6, 0x40, 0x14, 0,
          0x48, 0x8B, 0x4B, 0x18,
          0xBA, 2, 0, 0, 0,
          0x45, 0x33, 0xC0,
          0xFF, 0x53, 0x20,
          0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
          0x8B, 0x0B, 0x48, 0x8B, 0x04, 0xC8,
          0x48, 0x89, 0x43, 0x50,
          0x0F, 0xB6, 0x48, 0x14,
          0x48, 0x89, 0x4B, 0x60});
#else
    if (run_process_attach_callbacks) {
        emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
              0x45, 0x33, 0xC0, 0xFF, 0x53, 0x20});
        emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
              0x45, 0x33, 0xC0, 0xFF, 0x53, 0x28});
        emit({0x48, 0x8B, 0x4B, 0x18, 0xBA, 1, 0, 0, 0,
              0x45, 0x33, 0xC0, 0xFF, 0x53, 0x30});
        emit({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
              0x8B, 0x0B, 0x48, 0x8B, 0x04, 0xC8});
    }
    emit({0x48, 0x89, 0x43, 0x50,
          0x0F, 0xB6, 0x48, 0x14,
          0x48, 0x89, 0x4B, 0x58,
          0xC6, 0x40, 0x14, 0,
          0x0F, 0xB6, 0x48, 0x14,
          0x48, 0x89, 0x4B, 0x60});
#endif
    emit({
          0x65, 0x48, 0x8B, 0x04, 0x25, 0x08, 0, 0, 0,
          0x48, 0x89, 0x43, 0x78,
          0x65, 0x48, 0x8B, 0x04, 0x25, 0x10, 0, 0, 0,
          0x48, 0x89, 0x83, 0x80, 0, 0, 0,
          0x65, 0x48, 0x8B, 0x04, 0x25, 0x78, 0x14, 0, 0,
          0x48, 0x89, 0x83, 0x88, 0, 0, 0,
          0x48, 0xC7, 0x43, 0x40, 2, 0, 0, 0,
          0x48, 0x83, 0x7B, 0x40, 3, 0x74, 4,
          0xF3, 0x90, 0xEB, 0xF5,
          0x48, 0xC7, 0x43, 0x40, 1, 0, 0, 0,
          0x48, 0x8B, 0x53, 0x38,
          0x48, 0x83, 0xC4, 0x20, 0x5B});
#ifdef KIRKWARE_PARK_THREAD
    emit({0x48, 0x8D, 0x05, 0, 0, 0, 0});
    const std::size_t park_displacement = code.size() - 4;
    emit({0x48, 0x89, 0x04, 0x24,
          0x0F, 0x20, 0xC0, 0x33, 0xC0, 0xC3});
    const std::size_t park = code.size();
    emit({0x48, 0x83, 0xEC, 0x20,
          0xB9, 0xFF, 0xFF, 0xFF, 0xFF,
          0x48, 0xB8});
    const auto* sleep_bytes =
        reinterpret_cast<const std::uint8_t*>(&sleep_address);
    code.insert(code.end(), sleep_bytes,
                sleep_bytes + sizeof(sleep_address));
    emit({0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0xEB, 0});
    const auto loop_relative = static_cast<std::int8_t>(
        static_cast<std::ptrdiff_t>(park) -
        static_cast<std::ptrdiff_t>(code.size()));
    code.back() = static_cast<std::uint8_t>(loop_relative);
    const auto relative = static_cast<std::int32_t>(
        static_cast<std::ptrdiff_t>(park) -
        static_cast<std::ptrdiff_t>(park_displacement + 4));
    std::memcpy(code.data() + park_displacement, &relative,
                sizeof(relative));
#else
    (void)sleep_address;
    emit({0x0F, 0x20, 0xC0, 0x33, 0xC0, 0xC3});
#endif
    return code;
}

std::vector<std::uint8_t> BuildSentinelWatcherStub(
    std::uint64_t sleep_address) {
    std::vector<std::uint8_t> code;
    std::vector<std::size_t> failure_displacements;
    const auto emit = [&code](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    const auto patch = [&code](std::size_t displacement,
                               std::size_t target) {
        const auto relative = static_cast<std::int32_t>(
            static_cast<std::int64_t>(target) -
            static_cast<std::int64_t>(displacement +
                                      sizeof(std::int32_t)));
        std::memcpy(code.data() + displacement, &relative,
                    sizeof(relative));
    };
    const auto emit_jump = [&code, &emit, &patch](std::size_t target) {
        emit({0xE9, 0, 0, 0, 0});
        patch(code.size() - sizeof(std::int32_t), target);
    };
    emit({0x53, 0x48, 0x8B, 0xD9,
          0x48, 0xC7, 0x43, 0x08, 2, 0, 0, 0});
    const std::size_t wait_slot = code.size();
    emit({0x48, 0x8B, 0x03, 0x48, 0x8B, 0x00,
          0x48, 0x85, 0xC0, 0x0F, 0x85});
    const std::size_t node_displacement = code.size();
    emit({0, 0, 0, 0, 0xF3, 0x90});
    emit_jump(wait_slot);
    const std::size_t node_ready = code.size();
    emit({0xA8, 0x07, 0x0F, 0x85});
    failure_displacements.push_back(code.size());
    emit({0, 0, 0, 0, 0x48, 0x89, 0x43, 0x10});
    const std::size_t wait_back_link = code.size();
    emit({0x48, 0x8B, 0x50, 0x08, 0x48, 0x39, 0xC2, 0x0F, 0x84});
    const std::size_t back_link_displacement = code.size();
    emit({0, 0, 0, 0, 0xF3, 0x90});
    emit_jump(wait_back_link);
    const std::size_t back_link_ready = code.size();
    emit({0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x0F, 0x84});
    const std::size_t repair_displacement = code.size();
    emit({0, 0, 0, 0, 0x48, 0x39, 0xC2, 0x0F, 0x85});
    failure_displacements.push_back(code.size());
    emit({0, 0, 0, 0, 0xE9});
    const std::size_t verify_displacement = code.size();
    emit({0, 0, 0, 0});
    const std::size_t repair = code.size();
    emit({0x48, 0x89, 0x00});
    const std::size_t verify = code.size();
    emit({0x48, 0x39, 0x00, 0x0F, 0x85});
    failure_displacements.push_back(code.size());
    emit({0, 0, 0, 0, 0x48, 0x39, 0x40, 0x08, 0x0F, 0x85});
    failure_displacements.push_back(code.size());
#ifdef KIRKWARE_PARK_THREAD
    std::vector<std::size_t> park_displacements;
    emit({0, 0, 0, 0,
          0x48, 0xC7, 0x43, 0x08, 1, 0, 0, 0,
          0xB8, 1, 0, 0, 0, 0x5B, 0xE9, 0, 0, 0, 0});
    park_displacements.push_back(code.size() - 4);
    const std::size_t failure = code.size();
    emit({0x48, 0xC7, 0x43, 0x08, 0xFF, 0xFF, 0xFF, 0xFF,
          0x33, 0xC0, 0x5B, 0xE9, 0, 0, 0, 0});
    park_displacements.push_back(code.size() - 4);
    const std::size_t park = code.size();
    emit({0x48, 0x83, 0xEC, 0x28,
          0xB9, 0xFF, 0xFF, 0xFF, 0xFF,
          0x48, 0xB8});
    const auto* sleep_bytes =
        reinterpret_cast<const std::uint8_t*>(&sleep_address);
    code.insert(code.end(), sleep_bytes,
                sleep_bytes + sizeof(sleep_address));
    emit({0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xEB, 0});
    code.back() = static_cast<std::uint8_t>(static_cast<std::int8_t>(
        static_cast<std::ptrdiff_t>(park) -
        static_cast<std::ptrdiff_t>(code.size())));
    for (const auto displacement : park_displacements)
        patch(displacement, park);
#else
    (void)sleep_address;
    emit({0, 0, 0, 0,
          0x48, 0xC7, 0x43, 0x08, 1, 0, 0, 0,
          0xB8, 1, 0, 0, 0, 0x5B, 0xC3});
    const std::size_t failure = code.size();
    emit({0x48, 0xC7, 0x43, 0x08, 0xFF, 0xFF, 0xFF, 0xFF,
          0x33, 0xC0, 0x5B, 0xC3});
#endif
    patch(node_displacement, node_ready);
    patch(back_link_displacement, back_link_ready);
    patch(repair_displacement, repair);
    patch(verify_displacement, verify);
    for (const auto displacement : failure_displacements)
        patch(displacement, failure);
    return code;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseBarrierArgument(const char* value, HANDLE& ready,
                          HANDLE& release, HANDLE& abort,
                          HANDLE& success) {
    constexpr char prefix[] = "barrier:";
    if (!value || std::strncmp(value, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char* cursor = value + sizeof(prefix) - 1;
    const auto parse = [&cursor](HANDLE& handle, char separator) {
        char* end = nullptr;
        const auto raw = std::strtoull(cursor, &end, 16);
        if (!raw || end == cursor || *end != separator)
            return false;
        handle = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(raw));
        cursor = separator == '\0' ? end : end + 1;
        DWORD flags = 0;
        return GetHandleInformation(handle, &flags) != FALSE;
    };
    return parse(ready, ':') && parse(release, ':') &&
           parse(abort, ':') && parse(success, '\0') &&
           ready != release && ready != abort && ready != success &&
           release != abort && release != success && abort != success;
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::uint64_t RemoteModuleBase(DWORD pid, const std::string& requested) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto wanted = Lower(requested);
    std::uint64_t result = 0;
    if (Module32First(snapshot, &entry)) {
        do {
            if (Lower(entry.szModule) == wanted ||
                Lower(BaseName(entry.szExePath)) == wanted) {
                result = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool RemoteLoaderContainsBase(DWORD pid, std::uint64_t image_base) {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32First(snapshot, &entry)) {
        do {
            if (reinterpret_cast<std::uint64_t>(entry.modBaseAddr) ==
                image_base) {
                found = true;
                break;
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool RemoteImageIsRegisteredHybrid(HANDLE process,
                                   std::uint64_t image_base) {
    MEMORY_BASIC_INFORMATION memory{};
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::uint64_t callbacks[4]{};
    SIZE_T read = 0;
    if (!process ||
        !VirtualQueryEx(process,
                        reinterpret_cast<const void*>(image_base),
                        &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_MAPPED ||
        reinterpret_cast<std::uint64_t>(memory.AllocationBase) !=
            image_base ||
        !ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kTlsIndexRva),
            &tls_index, sizeof(tls_index), &read) ||
        read != sizeof(tls_index) || tls_index == TLS_OUT_OF_INDEXES ||
        !ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + 0x590340),
            callbacks, sizeof(callbacks), &read) ||
        read != sizeof(callbacks))
        return false;
    const std::uint64_t expected[4] = {
        image_base + 0x49C3FC, image_base + 0x4EFF80,
        image_base + 0x49C464, 0};
    return std::memcmp(callbacks, expected, sizeof(expected)) == 0;
}

std::uint64_t ResolveRemoteExport(DWORD pid, const char* requested_module,
                                  const char* symbol) {
    HMODULE requested = GetModuleHandleA(requested_module);
    if (!requested)
        requested = LoadLibraryExA(requested_module, nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!requested)
        return 0;
    FARPROC address = GetProcAddress(requested, symbol);
    if (!address)
        return 0;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(address), &owner))
        return 0;
    char owner_path[MAX_PATH]{};
    if (!GetModuleFileNameA(owner, owner_path, MAX_PATH))
        return 0;
    const auto remote_owner = RemoteModuleBase(pid, BaseName(owner_path));
    if (!remote_owner)
        return 0;
    return remote_owner +
           (reinterpret_cast<std::uint64_t>(address) -
            reinterpret_cast<std::uint64_t>(owner));
}

std::vector<std::uint8_t> BuildHandler() {
    return {
        0x48, 0x8B, 0x01, 0x81, 0x38, 0x96, 0x00, 0x00, 0xC0, 0x75, 0x55,
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0x39, 0x50, 0x10, 0x75, 0x45,
        0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x81, 0x88, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x81, 0xB8, 0x00, 0x00, 0x00, 0x48, 0xB8, 0, 0, 0, 0,
        0, 0, 0, 0, 0x48, 0x89, 0x81, 0x80, 0x00, 0x00, 0x00, 0x48, 0xB8,
        0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x89, 0x81, 0xF8, 0x00, 0x00, 0x00,
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0xC7, 0x81, 0x88, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3};
}

std::vector<std::uint8_t> BuildReturnCaptureHandler(
    std::uint64_t state_address, std::uint64_t return_address,
    std::uint64_t sleep_address, bool force_success) {
    if (!sleep_address)
        return {};
    std::vector<std::uint8_t> code;
    const auto emit = [&code](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    const auto emit64 = [&code](std::uint64_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        code.insert(code.end(), bytes, bytes + sizeof(value));
    };
    std::vector<std::size_t> search_jumps;

    emit({0x48, 0x8B, 0x11});
    emit({0x81, 0x3A, 0x03, 0x00, 0x00, 0x80});
    search_jumps.push_back(code.size());
    emit({0x75, 0x00});
    emit({0x4C, 0x8B, 0x42, 0x10});
    emit({0x48, 0xB8});
    emit64(return_address);
    emit({0x4C, 0x39, 0xC0});
    search_jumps.push_back(code.size());
    emit({0x75, 0x00});
    emit({0x48, 0x8B, 0x51, 0x08});
    if (force_success) {
        emit({0xB8, 0x01, 0x00, 0x00, 0x00});
        emit({0x48, 0x89, 0x42, 0x78});
    } else {
        emit({0x8B, 0x42, 0x78});
    }
    emit({0x48, 0x89, 0x82, 0x90, 0x00, 0x00, 0x00});
    emit({0x48, 0xB8});
    emit64(return_address + 2);
    emit({0x48, 0x89, 0x82, 0xF8, 0x00, 0x00, 0x00});
    emit({0x49, 0xB8});
    emit64(state_address);
    emit({0x49, 0x89, 0x50, 0x10});
    emit({0x49, 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00});
    const std::size_t wait = code.size();
    emit({0x49, 0xB8});
    emit64(state_address);
    emit({0x49, 0x83, 0x78, 0x08, 0x00});
    const std::size_t release_jump = code.size();
    emit({0x75, 0x00});
    emit({0x48, 0x83, 0xEC, 0x28,
          0xB9, 0xE8, 0x03, 0x00, 0x00,
          0x48, 0xB8});
    emit64(sleep_address);
    emit({0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28});
    const std::size_t wait_jump = code.size();
    emit({0xE9, 0x00, 0x00, 0x00, 0x00});
    const std::size_t release = code.size();
    emit({0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3});
    const auto search = code.size();
    emit({0x33, 0xC0, 0xC3});

    const auto release_displacement =
        static_cast<std::ptrdiff_t>(release) -
        static_cast<std::ptrdiff_t>(release_jump + 2);
    if (release_displacement < -128 || release_displacement > 127)
        return {};
    code[release_jump + 1] =
        static_cast<std::uint8_t>(release_displacement);
    const auto wait_displacement = static_cast<std::int32_t>(
        static_cast<std::ptrdiff_t>(wait) -
        static_cast<std::ptrdiff_t>(wait_jump + 5));
    std::memcpy(code.data() + wait_jump + 1, &wait_displacement,
                sizeof(wait_displacement));

    for (const auto jump : search_jumps) {
        const auto displacement = static_cast<std::ptrdiff_t>(search) -
                                  static_cast<std::ptrdiff_t>(jump + 2);
        if (displacement < -128 || displacement > 127)
            return {};
        code[jump + 1] = static_cast<std::uint8_t>(displacement);
    }
    return code;
}

std::vector<std::uint8_t> ReadBytes(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input.good())
        return {};
    return bytes;
}

bool Write(HANDLE process, void* address, const void* data, std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, address, data, size, &written) != FALSE &&
           written == size;
}

bool WriteProtected(HANDLE process, std::uint64_t address,
                    const void* data, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address), size,
                          PAGE_EXECUTE_READWRITE, &old))
        return false;
    const bool written =
        Write(process, reinterpret_cast<void*>(address), data, size);
    DWORD ignored = 0;
    VirtualProtectEx(process, reinterpret_cast<void*>(address), size, old,
                     &ignored);
    return written;
}

bool InstallFixedTimeStub(HANDLE process, std::uint64_t image_base,
                          std::uint64_t unix_time,
                          std::uint64_t* prior_slot,
                          std::uint64_t* stub_address,
                          std::uint64_t* verified_slot) {
    const auto slot = image_base + kResolvedTimeSlotRva;
    const auto store = image_base + kResolvedTimeStoreRva;
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(slot),
                           prior_slot, sizeof(*prior_slot), &read) ||
        read != sizeof(*prior_slot))
        return false;
    const std::uint64_t filetime =
        (unix_time + 11644473600ull) * 10000000ull;
    std::uint8_t code[] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0x89, 0x01, 0xC3,
    };
    std::memcpy(code + 2, &filetime, sizeof(filetime));
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!remote || !Write(process, remote, code, sizeof(code)))
        return false;
    FlushInstructionCache(process, remote, sizeof(code));
    constexpr std::uint8_t expected_store[] = {
        0x48, 0x89, 0x05, 0xD9, 0xEF, 0x33, 0x00,
    };
    constexpr std::uint8_t nop_store[] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };
    std::uint8_t observed_store[sizeof(expected_store)]{};
    read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(store),
                           observed_store, sizeof(observed_store), &read) ||
        read != sizeof(observed_store) ||
        std::memcmp(observed_store, expected_store,
                    sizeof(expected_store)) != 0 ||
        !WriteProtected(process, store, nop_store, sizeof(nop_store)))
        return false;
    FlushInstructionCache(process, reinterpret_cast<const void*>(store),
                          sizeof(nop_store));
    *stub_address = reinterpret_cast<std::uint64_t>(remote);
    if (!WriteProtected(process, slot, stub_address, sizeof(*stub_address)))
        return false;
    read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(slot),
                             verified_slot, sizeof(*verified_slot), &read) &&
           read == sizeof(*verified_slot) &&
           *verified_slot == *stub_address;
}

} 

int main(int argc, char** argv) {
    if (argc != 6 && argc != 7 && argc != 8) {
        std::fprintf(stderr,
                     "usage: invoke_kirkware_veh.exe <pid> <module-name> <blob-hex> <mbcinfo-template> <loader-context-page> [blob|context|envelope|null] [barrier:ready:release:abort:success]\n");
        return 2;
    }
    const bool return_barrier_enabled = argc == 8;
    HANDLE barrier_ready_event = nullptr;
    HANDLE barrier_release_event = nullptr;
    HANDLE barrier_abort_event = nullptr;
    HANDLE barrier_success_event = nullptr;
    if (return_barrier_enabled &&
        !ParseBarrierArgument(argv[7], barrier_ready_event,
                              barrier_release_event,
                              barrier_abort_event,
                              barrier_success_event)) {
        std::fprintf(stderr, "invalid return barrier handles\n");
        return 2;
    }
    const char* fixed_time_text = std::getenv("KIRKWARE_FIXED_UNIX");
    const std::uint64_t fixed_unix_time =
        fixed_time_text ? std::strtoull(fixed_time_text, nullptr, 0) : 0;
    const DWORD pid = std::strtoul(argv[1], nullptr, 10);
    const auto image_base =
        _strnicmp(argv[2], "0x", 2) == 0
            ? std::strtoull(argv[2] + 2, nullptr, 16)
            : RemoteModuleBase(pid, argv[2]);
    const auto blob = std::strtoull(argv[3], nullptr, 16);
    auto handler = BuildHandler();
    auto mbcinfo = ReadBytes(argv[4]);
    auto loader_context = ReadBytes(argv[5]);
    const bool nested_context = loader_context.size() == 0x30298;
    if (!image_base || !blob || handler.size() != kHandlerSize ||
        mbcinfo.size() < 0x220 ||
        (!nested_context && loader_context.size() != 0x1000)) {
        std::fprintf(stderr,
                     "invalid module, blob, locale, or context input\n");
        return 1;
    }
    mbcinfo.resize(0x220);
    const std::uint32_t permanent_reference = 0x7FFFFFFF;
    std::memcpy(mbcinfo.data(), &permanent_reference,
                sizeof(permanent_reference));
    const std::uint64_t entry = image_base + 0x49C8E0;
    if (!nested_context)
        std::memcpy(loader_context.data() + 0x30, &image_base,
                    sizeof(image_base));
    std::memcpy(handler.data() + kHandlerImageBaseOffset, &image_base,
                sizeof(image_base));
    std::memcpy(handler.data() + kHandlerEntryOffset, &entry, sizeof(entry));
    const auto add_veh = ResolveRemoteExport(pid, "kernel32.dll",
                                             "AddVectoredExceptionHandler");
    if (!add_veh) {
        std::fprintf(stderr, "AddVectoredExceptionHandler resolution failed\n");
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_SUSPEND_RESUME |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ | SYNCHRONIZE,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    const bool public_loader_managed =
        RemoteLoaderContainsBase(pid, image_base);
    const bool registered_hybrid =
        RemoteImageIsRegisteredHybrid(process, image_base);
#ifdef KIRKWARE_EXTERNAL_STATIC_TLS_CARRIER
    const bool external_static_tls_carrier =
        !public_loader_managed &&
        RemoteLoaderContainsBase(pid, kExternalTlsCarrierBase);
#else
    const bool external_static_tls_carrier = false;
#endif
    const bool loader_managed =
#if defined(KIRKWARE_FORCE_UNLINKED_MANUAL)
        public_loader_managed || external_static_tls_carrier;
#else
        public_loader_managed || registered_hybrid ||
        external_static_tls_carrier;
#endif
#if defined(KIRKWARE_UNLINKED_STATIC_TLS_MANUAL_CALLBACKS)
    const bool unlinked_static_tls_manual_callbacks =
        !public_loader_managed && registered_hybrid;
#else
    const bool unlinked_static_tls_manual_callbacks = false;
#endif
    std::uint64_t park_sleep = 0;
#ifdef KIRKWARE_PARK_THREAD
    park_sleep = ResolveRemoteExport(pid, "kernel32.dll", "Sleep");
    if (!park_sleep) {
        std::fprintf(stderr, "remote Sleep resolution failed\n");
        CloseHandle(process);
        return 1;
    }
#endif
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::uint64_t tls_set_value = 0;
    std::uint64_t tls_block_address = 0;
    SIZE_T tls_read = 0;
#ifdef KIRKWARE_EXTERNAL_STATIC_TLS_CARRIER
    DWORD carrier_tls_index = TLS_OUT_OF_INDEXES;
    if (!external_static_tls_carrier ||
        !ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(kExternalTlsCarrierBase +
                                          kTlsIndexRva),
            &carrier_tls_index, sizeof(carrier_tls_index), &tls_read) ||
        tls_read != sizeof(carrier_tls_index) ||
        carrier_tls_index == TLS_OUT_OF_INDEXES ||
        !Write(process,
               reinterpret_cast<void*>(image_base + kTlsIndexRva),
               &carrier_tls_index, sizeof(carrier_tls_index))) {
        std::fprintf(stderr,
                     "external static TLS carrier is unavailable\n");
        CloseHandle(process);
        return 1;
    }
#endif
    std::vector<std::uint8_t> tls_data(kTlsDataSize);
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kTlsIndexRva),
            &tls_index, sizeof(tls_index), &tls_read) ||
        tls_read != sizeof(tls_index) ||
        tls_index == TLS_OUT_OF_INDEXES ||
        !ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kTlsDataRva),
            tls_data.data(), tls_data.size(), &tls_read) ||
        tls_read != tls_data.size()) {
        std::fprintf(stderr, "payload TLS metadata is unavailable\n");
        CloseHandle(process);
        return 1;
    }
    if (!loader_managed) {
        tls_set_value = ResolveRemoteExport(pid, "kernel32.dll",
                                            "TlsSetValue");
        void* tls_block = VirtualAllocEx(process, nullptr, tls_data.size(),
                                         MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
        if (!tls_set_value || !tls_block ||
            !Write(process, tls_block, tls_data.data(), tls_data.size())) {
            std::fprintf(stderr, "payload TLS block setup failed: %lu\n",
                         GetLastError());
            CloseHandle(process);
            return 1;
        }
        tls_block_address = reinterpret_cast<std::uint64_t>(tls_block);
    }
    auto* remote_mbcinfo = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, mbcinfo.size(), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (!remote_mbcinfo ||
        !Write(process, remote_mbcinfo, mbcinfo.data(), mbcinfo.size())) {
        std::fprintf(stderr, "locale allocation/write failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    std::uint64_t requested_handler = 0;
    std::uint64_t requested_context = 0;
    if (image_base == 0x000001E5DCC00000ull)
        requested_handler = 0x000001E5D24D0000ull,
        requested_context = 0x000001E5D2500000ull;
    else if (image_base == 0x000001E5DF940000ull)
        requested_handler = 0x000001E5D8520000ull,
        requested_context = 0x000001E5D8550000ull;
    else if (image_base == 0x000001D16B490000ull)
        requested_handler = 0x000001D163050000ull,
        requested_context = 0x000001D163080000ull;
    if (!requested_context) {
        std::fprintf(stderr, "unsupported image base for exact context mapping\n");
        CloseHandle(process);
        return 1;
    }
    auto* remote_context = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, reinterpret_cast<void*>(requested_context),
        loader_context.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!remote_context ||
        reinterpret_cast<std::uint64_t>(remote_context) != requested_context ||
        !Write(process, remote_context, loader_context.data(),
               loader_context.size())) {
        std::fprintf(stderr, "exact context allocation/write failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    auto* remote_handler = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, reinterpret_cast<void*>(requested_handler), 0x1000,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    auto* remote_register = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!remote_handler || !remote_register ||
        !Write(process, remote_handler, handler.data(), handler.size())) {
        std::fprintf(stderr, "handler allocation/write failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    const std::uint64_t handler_address =
        reinterpret_cast<std::uint64_t>(remote_handler);
    std::uint8_t register_stub[] = {
        0x48, 0x83, 0xEC, 0x28,
        0xB9, 0x01, 0x00, 0x00, 0x00,
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0xC3};
    std::memcpy(register_stub + 11, &handler_address, sizeof(handler_address));
    std::memcpy(register_stub + 21, &add_veh, sizeof(add_veh));
    if (!Write(process, remote_register, register_stub, sizeof(register_stub))) {
        std::fprintf(stderr, "registration stub write failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    HANDLE registration = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_register), nullptr, 0,
        nullptr);
    if (!registration || WaitForSingleObject(registration, 30000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "handler registration thread failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    DWORD registration_result = 0;
    GetExitCodeThread(registration, &registration_result);
    CloseHandle(registration);
    if (!registration_result) {
        std::fprintf(stderr, "AddVectoredExceptionHandler returned null\n");
        CloseHandle(process);
        return 1;
    }

    ReturnCaptureState* remote_return_capture = nullptr;
    std::uint8_t* remote_capture_handler = nullptr;
    std::uint8_t return_instruction[2]{};
    const auto return_address = image_base + 0x49C842;
    bool return_breakpoint_installed = false;
    bool return_patch_attempted = false;
    DWORD return_original_protection = 0;

    std::uint32_t startup_state = 0;
    SIZE_T state_read = 0;
    const auto startup_state_address = image_base + 0x7E41D0;
    const auto locale_global = image_base + 0x6F9400;
    std::uint64_t captured_locale = 0;
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const void*>(startup_state_address),
                           &startup_state, sizeof(startup_state), &state_read) ||
        state_read != sizeof(startup_state) || startup_state != 0 ||
        !ReadProcessMemory(process,
                           reinterpret_cast<const void*>(locale_global),
                           &captured_locale, sizeof(captured_locale),
                           &state_read) ||
        state_read != sizeof(captured_locale)) {
        std::fprintf(stderr, "unexpected CRT startup state\n");
        CloseHandle(process);
        return 1;
    }
    auto* remote_poller = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    std::vector<std::uint8_t> poller = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xC7, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,
        0x83, 0x39, 0x00,
        0x74, 0xFB,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,
        0x83, 0x39, 0x02,
        0x75, 0xFB,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,
        0x33, 0xC0,
        0xC3};
#ifdef KIRKWARE_PARK_THREAD
    poller.pop_back();
    const std::size_t poller_park = poller.size();
    const std::uint8_t poller_park_prefix[] = {
        0x48, 0x83, 0xEC, 0x28,
        0xB9, 0xFF, 0xFF, 0xFF, 0xFF,
        0x48, 0xB8,
    };
    poller.insert(poller.end(), poller_park_prefix,
                  poller_park_prefix + sizeof(poller_park_prefix));
    const auto* poller_sleep_bytes =
        reinterpret_cast<const std::uint8_t*>(&park_sleep);
    poller.insert(poller.end(), poller_sleep_bytes,
                  poller_sleep_bytes + sizeof(park_sleep));
    const std::uint8_t poller_park_suffix[] = {
        0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xEB, 0,
    };
    poller.insert(poller.end(), poller_park_suffix,
                  poller_park_suffix + sizeof(poller_park_suffix));
    poller.back() = static_cast<std::uint8_t>(static_cast<std::int8_t>(
        static_cast<std::ptrdiff_t>(poller_park) -
        static_cast<std::ptrdiff_t>(poller.size())));
#endif
    const auto mbcinfo_address =
        reinterpret_cast<std::uint64_t>(remote_mbcinfo);
    if (!captured_locale)
        captured_locale = mbcinfo_address;
    const auto poller_ready_address =
        reinterpret_cast<std::uint64_t>(remote_poller + 0x100);
    std::memcpy(poller.data() + 2, &poller_ready_address,
                sizeof(poller_ready_address));
    std::memcpy(poller.data() + 18, &startup_state_address,
                sizeof(startup_state_address));
    std::memcpy(poller.data() + 33, &mbcinfo_address,
                sizeof(mbcinfo_address));
    std::memcpy(poller.data() + 43, &locale_global,
                sizeof(locale_global));
    std::memcpy(poller.data() + 58, &captured_locale,
                sizeof(captured_locale));
    std::memcpy(poller.data() + 68, &locale_global,
                sizeof(locale_global));
    if (!remote_poller ||
        !Write(process, remote_poller, poller.data(), poller.size())) {
        std::fprintf(stderr, "late locale poller creation failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    FlushInstructionCache(process, remote_poller, poller.size());
    HANDLE poll_thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_poller), nullptr, 0,
        nullptr);
    if (!poll_thread) {
        std::fprintf(stderr, "late locale poller thread failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    SetThreadPriority(poll_thread, THREAD_PRIORITY_TIME_CRITICAL);
    std::uint32_t poller_ready = 0;
    const ULONGLONG ready_deadline = GetTickCount64() + 5000;
    do {
        SIZE_T ready_read = 0;
        if (!ReadProcessMemory(process, remote_poller + 0x100,
                               &poller_ready, sizeof(poller_ready),
                               &ready_read) ||
            ready_read != sizeof(poller_ready))
            break;
        if (poller_ready == 1)
            break;
        Sleep(1);
    } while (GetTickCount64() < ready_deadline);
    if (poller_ready != 1) {
        std::fprintf(stderr, "late locale poller did not become ready\n");
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }

    const auto context_address =
        reinterpret_cast<std::uint64_t>(remote_context);
    std::uint64_t entry_argument = blob;
    if (argc >= 7 && std::string(argv[6]) == "context")
        entry_argument = context_address;
    else if (argc >= 7 && std::string(argv[6]) == "envelope")
        entry_argument = requested_context - 0x70000;
    else if (argc >= 7 && std::string(argv[6]) == "null")
        entry_argument = 0;
    else if (argc >= 7 && std::string(argv[6]) != "blob") {
        std::fprintf(stderr, "invalid entry argument mode\n");
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    TriggerContext trigger_context{};
    trigger_context.tls_index = tls_index;
    trigger_context.tls_block = tls_block_address;
    trigger_context.tls_set_value = tls_set_value;
    trigger_context.image_base = image_base;
    trigger_context.entry_argument = entry_argument;
    std::vector<std::uint8_t> trigger;
    const std::size_t trigger_context_size =
        (sizeof(trigger_context) + 15u) & ~std::size_t{15u};
    if (loader_managed) {
#ifdef KIRKWARE_EXTERNAL_STATIC_TLS_CARRIER
        trigger_context.callbacks[0] = image_base + 0x49C3FC;
#endif
        if (unlinked_static_tls_manual_callbacks) {
            trigger_context.callbacks[0] = image_base + 0x49C3FC;
            trigger_context.callbacks[1] = image_base + 0x4EFF80;
            trigger_context.callbacks[2] = image_base + 0x49C464;
        }
        trigger = BuildLoaderManagedTriggerStub(
            park_sleep, unlinked_static_tls_manual_callbacks);
    } else {
        trigger_context.callbacks[0] = image_base + 0x49C3FC;
        trigger_context.callbacks[1] = image_base + 0x4EFF80;
        trigger_context.callbacks[2] = image_base + 0x49C464;
        trigger = BuildTriggerStub();
    }
    SentinelWatcherContext watcher_context{};
    watcher_context.slot_address = image_base + kSentinelSlotRva;
    const auto watcher = BuildSentinelWatcherStub(park_sleep);
    const std::size_t watcher_context_size =
        (sizeof(watcher_context) + 15u) & ~std::size_t{15u};
    auto* remote_watcher = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, watcher_context_size + watcher.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote_watcher ||
        !Write(process, remote_watcher, &watcher_context,
               sizeof(watcher_context)) ||
        !Write(process, remote_watcher + watcher_context_size,
               watcher.data(), watcher.size())) {
        std::fprintf(stderr, "sentinel watcher allocation/write failed: %lu\n",
                     GetLastError());
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    FlushInstructionCache(process, remote_watcher + watcher_context_size,
                          watcher.size());
    HANDLE watcher_thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            remote_watcher + watcher_context_size),
        remote_watcher, CREATE_SUSPENDED, nullptr);
    const DWORD watcher_resume_count =
        watcher_thread ? ResumeThread(watcher_thread)
                       : static_cast<DWORD>(-1);
    if (!watcher_thread ||
        !SetThreadPriority(watcher_thread, THREAD_PRIORITY_TIME_CRITICAL) ||
        watcher_resume_count != 1) {
        std::fprintf(stderr, "sentinel watcher thread failed: %lu\n",
                     GetLastError());
        if (watcher_thread) {
            TerminateThread(watcher_thread, 0);
            WaitForSingleObject(watcher_thread, 5000);
            CloseHandle(watcher_thread);
        }
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    const auto remote_watcher_status =
        remote_watcher + offsetof(SentinelWatcherContext, status);
    std::uint64_t watcher_status = 0;
    SIZE_T watcher_read = 0;
    const ULONGLONG watcher_ready_deadline = GetTickCount64() + 5000;
    do {
        if (!ReadProcessMemory(process, remote_watcher_status,
                               &watcher_status, sizeof(watcher_status),
                               &watcher_read) ||
            watcher_read != sizeof(watcher_status))
            break;
        if (watcher_status != 0)
            break;
        Sleep(1);
    } while (GetTickCount64() < watcher_ready_deadline);
    if (watcher_status != 2 && watcher_status != 1) {
        std::fprintf(stderr, "sentinel watcher did not become ready: 0x%llX\n",
                     static_cast<unsigned long long>(watcher_status));
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        CloseHandle(watcher_thread);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    auto* remote_trigger = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, trigger_context_size + trigger.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote_trigger ||
        !Write(process, remote_trigger, &trigger_context,
               sizeof(trigger_context)) ||
        !Write(process, remote_trigger + trigger_context_size,
               trigger.data(), trigger.size())) {
        std::fprintf(stderr, "trigger allocation/write failed: %lu\n",
                     GetLastError());
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        CloseHandle(watcher_thread);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    FlushInstructionCache(process, remote_trigger + trigger_context_size,
                          trigger.size());
    constexpr std::uint8_t privileged_instruction[] = {0x0F, 0x20, 0xC0};
    const auto trigger_pc_it = std::search(
        trigger.begin(), trigger.end(), std::begin(privileged_instruction),
        std::end(privileged_instruction));
    const bool unique_trigger_pc =
        trigger_pc_it != trigger.end() &&
        std::search(trigger_pc_it + sizeof(privileged_instruction),
                    trigger.end(), std::begin(privileged_instruction),
                    std::end(privileged_instruction)) == trigger.end();
    const std::uint64_t trigger_pc =
        unique_trigger_pc
            ? reinterpret_cast<std::uint64_t>(remote_trigger) +
                  trigger_context_size +
                  static_cast<std::size_t>(trigger_pc_it - trigger.begin())
            : 0;
    if (!trigger_pc ||
        !Write(process, remote_handler + kHandlerTriggerPcOffset,
               &trigger_pc, sizeof(trigger_pc))) {
        std::fprintf(stderr, "bootstrap exception filter setup failed: %lu\n",
                     GetLastError());
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        CloseHandle(watcher_thread);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    FlushInstructionCache(process, remote_handler, handler.size());
    const SIZE_T trigger_stack_size =
        static_cast<SIZE_T>(KIRKWARE_TRIGGER_STACK_SIZE);
    const DWORD trigger_creation_flags =
        CREATE_SUSPENDED |
        (trigger_stack_size != 0 ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0);
    HANDLE thread = CreateRemoteThread(
        process, nullptr, trigger_stack_size,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            remote_trigger + trigger_context_size),
        remote_trigger,
        trigger_creation_flags, nullptr);
    if (!thread) {
        std::fprintf(stderr, "trigger thread failed: %lu\n", GetLastError());
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        CloseHandle(watcher_thread);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    const DWORD trigger_resume_count = ResumeThread(thread);
    if (trigger_resume_count != 1) {
        std::fprintf(stderr,
                     "trigger thread resume invariant failed: prior=%lu error=%lu\n",
                     trigger_resume_count, GetLastError());
        TerminateThread(thread, 0);
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        CloseHandle(watcher_thread);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    const auto stop_owned_threads = [&]() {
        TerminateThread(thread, 0);
        WaitForSingleObject(thread, 5000);
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
        TerminateThread(poll_thread, 0);
        WaitForSingleObject(poll_thread, 5000);
    };
#ifdef KIRKWARE_EXPAND_STATIC_TLS_VECTOR
    const ULONGLONG tls_vector_ready_deadline = GetTickCount64() + 10000;
    SIZE_T tls_vector_read = 0;
    do {
        if (!ReadProcessMemory(process, remote_trigger, &trigger_context,
                               sizeof(trigger_context), &tls_vector_read) ||
            tls_vector_read != sizeof(trigger_context))
            break;
        if (trigger_context.status == 4)
            break;
        Sleep(1);
    } while (GetTickCount64() < tls_vector_ready_deadline);
    const std::uint64_t old_tls_vector = trigger_context.tls_slot_after_set;
    MEMORY_BASIC_INFORMATION old_tls_memory{};
    const std::size_t required_tls_vector_size =
        (static_cast<std::size_t>(tls_index) + 1u) *
        sizeof(std::uint64_t);
    const std::size_t expanded_tls_vector_size =
        (std::max<std::size_t>(required_tls_vector_size, 0x1000u) +
         0xFFFu) & ~std::size_t{0xFFFu};
    std::vector<std::uint8_t> expanded_tls_vector(
        expanded_tls_vector_size, 0);
    bool expanded_tls_ready =
        trigger_context.status == 4 && old_tls_vector != 0 &&
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(old_tls_vector),
                       &old_tls_memory, sizeof(old_tls_memory)) ==
            sizeof(old_tls_memory) &&
        old_tls_memory.State == MEM_COMMIT &&
        reinterpret_cast<std::uint64_t>(old_tls_memory.BaseAddress) <=
            old_tls_vector;
    if (expanded_tls_ready) {
        const auto region_base = reinterpret_cast<std::uint64_t>(
            old_tls_memory.BaseAddress);
        const auto region_offset = static_cast<std::size_t>(
            old_tls_vector - region_base);
        const auto available =
            region_offset < old_tls_memory.RegionSize
                ? old_tls_memory.RegionSize - region_offset
                : 0;
        const auto copy_size = std::min(expanded_tls_vector_size,
                                        static_cast<std::size_t>(available));
        SIZE_T copied = 0;
        expanded_tls_ready = copy_size != 0 &&
            ReadProcessMemory(
                process, reinterpret_cast<const void*>(old_tls_vector),
                expanded_tls_vector.data(), copy_size, &copied) != FALSE &&
            copied == copy_size;
    }
    std::memcpy(expanded_tls_vector.data() +
                    static_cast<std::size_t>(tls_index) *
                        sizeof(std::uint64_t),
                &tls_block_address, sizeof(tls_block_address));
    auto* remote_expanded_tls_vector =
        expanded_tls_ready
            ? static_cast<std::uint8_t*>(VirtualAllocEx(
                  process, nullptr, expanded_tls_vector.size(),
                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
            : nullptr;
    const auto expanded_tls_vector_address =
        reinterpret_cast<std::uint64_t>(remote_expanded_tls_vector);
    const std::uint64_t install_tls_vector = 5;
    expanded_tls_ready = expanded_tls_ready &&
        remote_expanded_tls_vector != nullptr &&
        Write(process, remote_expanded_tls_vector,
              expanded_tls_vector.data(), expanded_tls_vector.size()) &&
        Write(process,
              remote_trigger +
                  offsetof(TriggerContext, tls_slot_after_callbacks),
              &expanded_tls_vector_address,
              sizeof(expanded_tls_vector_address)) &&
        Write(process,
              remote_trigger + offsetof(TriggerContext, status),
              &install_tls_vector, sizeof(install_tls_vector));
    if (!expanded_tls_ready) {
        std::fprintf(stderr,
                     "static TLS vector expansion failed: old=0x%llX "
                     "index=%lu error=%lu\n",
                     static_cast<unsigned long long>(old_tls_vector),
                     tls_index, GetLastError());
        stop_owned_threads();
        CloseHandle(thread);
        CloseHandle(watcher_thread);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    std::printf(
        "tls_vector old=0x%llX expanded=0x%llX bytes=0x%llX index=%lu\n",
        static_cast<unsigned long long>(old_tls_vector),
        static_cast<unsigned long long>(expanded_tls_vector_address),
        static_cast<unsigned long long>(expanded_tls_vector_size),
        tls_index);
#endif
    const ULONGLONG tls_boundary_deadline = GetTickCount64() + 10000;
    SIZE_T tls_boundary_read = 0;
    do {
        if (!ReadProcessMemory(process, remote_trigger, &trigger_context,
                               sizeof(trigger_context),
                               &tls_boundary_read) ||
            tls_boundary_read != sizeof(trigger_context))
            break;
#ifdef KIRKWARE_EXPAND_STATIC_TLS_VECTOR
        if (trigger_context.status == 2 ||
            trigger_context.status == UINT64_MAX)
#else
        if (trigger_context.status != 0)
#endif
            break;
        Sleep(1);
    } while (GetTickCount64() < tls_boundary_deadline);
    std::printf("tls_boundary loader_managed=%u external_static_tls_carrier=%u unlinked_static_tls_manual_callbacks=%u expected=0x%llX after_set=0x%llX after_callbacks=0x%llX flag_before=%llu flag_after=%llu status=0x%llX crt_init=0x%llX crt_ptd=0x%llX stack_base=0x%llX stack_limit=0x%llX deallocation_stack=0x%llX reserve=0x%llX\n",
                loader_managed ? 1u : 0u,
                external_static_tls_carrier ? 1u : 0u,
                unlinked_static_tls_manual_callbacks ? 1u : 0u,
                static_cast<unsigned long long>(tls_block_address),
                static_cast<unsigned long long>(
                    trigger_context.tls_slot_after_set),
                static_cast<unsigned long long>(
                    trigger_context.tls_slot_after_callbacks),
                static_cast<unsigned long long>(
                    trigger_context.tls_flag_before_reset),
                static_cast<unsigned long long>(
                    trigger_context.tls_flag_after_reset),
                static_cast<unsigned long long>(trigger_context.status),
                static_cast<unsigned long long>(
                    trigger_context.crt_init_result),
                static_cast<unsigned long long>(
                    trigger_context.crt_ptd_result),
                static_cast<unsigned long long>(trigger_context.stack_base),
                static_cast<unsigned long long>(trigger_context.stack_limit),
                static_cast<unsigned long long>(
                    trigger_context.deallocation_stack),
                static_cast<unsigned long long>(trigger_stack_size));
    const bool tls_boundary_valid =
        loader_managed
            ? trigger_context.tls_slot_after_set != 0 &&
                  (trigger_context.tls_slot_after_set & 0xFu) == 0 &&
                  trigger_context.tls_slot_after_callbacks ==
                      trigger_context.tls_slot_after_set
            : trigger_context.tls_slot_after_set == tls_block_address &&
                  trigger_context.tls_slot_after_callbacks ==
                      tls_block_address;
    const bool crt_thread_valid =
#ifdef KIRKWARE_CRT_THREAD_INIT
        trigger_context.crt_init_result != 0 &&
        trigger_context.crt_ptd_result != 0;
#else
        true;
#endif
    if (trigger_context.status != 2 || !tls_boundary_valid ||
        !crt_thread_valid) {
        std::fprintf(stderr, "payload TLS boundary verification failed\n");
        stop_owned_threads();
        CloseHandle(thread);
        CloseHandle(watcher_thread);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    std::uint64_t time_slot_prior = 0;
    std::uint64_t time_stub_address = 0;
    std::uint64_t time_slot_verified = 0;
    if (fixed_unix_time) {
        if (!InstallFixedTimeStub(process, image_base, fixed_unix_time,
                                  &time_slot_prior, &time_stub_address,
                                  &time_slot_verified)) {
            std::fprintf(stderr,
                         "post-callback fixed-time stub failed: %lu\n",
                         GetLastError());
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        std::printf(
            "time_stub_pre_entry slot=0x%llX prior=0x%llX stub=0x%llX "
            "verified=0x%llX fixed_unix=%llu store_nopped=0x%llX\n",
            static_cast<unsigned long long>(
                image_base + kResolvedTimeSlotRva),
            static_cast<unsigned long long>(time_slot_prior),
            static_cast<unsigned long long>(time_stub_address),
            static_cast<unsigned long long>(time_slot_verified),
            static_cast<unsigned long long>(fixed_unix_time),
            static_cast<unsigned long long>(
                image_base + kResolvedTimeStoreRva));
    }
    const auto restore_return_instruction = [&]() {
        if (!return_patch_attempted)
            return true;
        DWORD ignored_current_protection = 0;
        if (!VirtualProtectEx(
                process, reinterpret_cast<void*>(return_address),
                sizeof(return_instruction), PAGE_EXECUTE_READWRITE,
                &ignored_current_protection))
            return false;
        const bool wrote = Write(
            process, reinterpret_cast<void*>(return_address),
            return_instruction, sizeof(return_instruction));
        const bool flushed =
            FlushInstructionCache(
                process, reinterpret_cast<void*>(return_address),
                sizeof(return_instruction)) != FALSE;
        DWORD ignored_protection = 0;
        const bool protection_restored =
            VirtualProtectEx(
                process, reinterpret_cast<void*>(return_address),
                sizeof(return_instruction), return_original_protection,
                &ignored_protection) != FALSE;
        std::uint8_t readback[sizeof(return_instruction)]{};
        SIZE_T read = 0;
        const bool verified =
            ReadProcessMemory(
                process, reinterpret_cast<const void*>(return_address),
                readback, sizeof(readback), &read) != FALSE &&
            read == sizeof(readback) &&
            std::memcmp(readback, return_instruction,
                        sizeof(return_instruction)) == 0;
        if (wrote && flushed && protection_restored && verified) {
            return_breakpoint_installed = false;
            return_patch_attempted = false;
        }
        return !return_patch_attempted;
    };
    if (return_barrier_enabled) {
        if (WaitForSingleObject(barrier_abort_event, 0) == WAIT_OBJECT_0) {
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        remote_return_capture = static_cast<ReturnCaptureState*>(
            VirtualAllocEx(process, nullptr, 0x1000,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        const auto capture_state_address =
            reinterpret_cast<std::uint64_t>(remote_return_capture);
        const auto capture_handler = BuildReturnCaptureHandler(
            capture_state_address, return_address, park_sleep, false);
        remote_capture_handler = static_cast<std::uint8_t*>(
            VirtualAllocEx(process, nullptr, 0x1000,
                           MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE));
        if (!remote_return_capture || !remote_capture_handler ||
            capture_handler.empty() ||
            !Write(process, remote_capture_handler,
                   capture_handler.data(), capture_handler.size()) ||
            !FlushInstructionCache(process, remote_capture_handler,
                                   capture_handler.size())) {
            std::fprintf(stderr, "return barrier handler setup failed: %lu\n",
                         GetLastError());
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        const std::uint64_t capture_handler_address =
            reinterpret_cast<std::uint64_t>(remote_capture_handler);
        std::memcpy(register_stub + 11, &capture_handler_address,
                    sizeof(capture_handler_address));
        if (!Write(process, remote_register, register_stub,
                   sizeof(register_stub))) {
            std::fprintf(stderr, "return barrier registration write failed\n");
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        HANDLE capture_registration = CreateRemoteThread(
            process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_register),
            nullptr, 0, nullptr);
        DWORD capture_registration_result = 0;
        const bool registered =
            capture_registration &&
            WaitForSingleObject(capture_registration, 30000) ==
                WAIT_OBJECT_0 &&
            GetExitCodeThread(capture_registration,
                              &capture_registration_result) &&
            capture_registration_result != 0;
        if (capture_registration)
            CloseHandle(capture_registration);
        if (!registered) {
            std::fprintf(stderr,
                         "return barrier handler registration failed: %lu\n",
                         GetLastError());
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        SIZE_T return_read = 0;
        if (!ReadProcessMemory(
                process, reinterpret_cast<const void*>(return_address),
                return_instruction, sizeof(return_instruction), &return_read) ||
            return_read != sizeof(return_instruction) ||
            return_instruction[0] != 0x8B ||
            return_instruction[1] != 0xD8) {
            std::fprintf(stderr, "unexpected protected return instruction\n");
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        const std::uint8_t breakpoint = 0xCC;
        const bool writable = VirtualProtectEx(
            process, reinterpret_cast<void*>(return_address),
            sizeof(return_instruction), PAGE_EXECUTE_READWRITE,
            &return_original_protection) != FALSE;
        return_patch_attempted = writable;
        const bool patched = writable && Write(
            process, reinterpret_cast<void*>(return_address),
            &breakpoint, sizeof(breakpoint));
        const bool flushed = patched &&
            FlushInstructionCache(
                process, reinterpret_cast<void*>(return_address),
                sizeof(return_instruction)) != FALSE;
        DWORD ignored_protection = 0;
        const bool protection_restored = writable &&
            VirtualProtectEx(process,
                             reinterpret_cast<void*>(return_address),
                             sizeof(return_instruction),
                             return_original_protection,
                             &ignored_protection) != FALSE;
        const std::uint8_t patched_instruction[2] = {0xCC, 0xD8};
        std::uint8_t patched_readback[2]{};
        SIZE_T patched_read = 0;
        const bool patch_verified =
            ReadProcessMemory(
                process, reinterpret_cast<const void*>(return_address),
                patched_readback, sizeof(patched_readback),
                &patched_read) != FALSE &&
            patched_read == sizeof(patched_readback) &&
            std::memcmp(patched_readback, patched_instruction,
                        sizeof(patched_instruction)) == 0;
        return_breakpoint_installed =
            patched && flushed && protection_restored && patch_verified;
        if (!return_breakpoint_installed) {
            restore_return_instruction();
            std::fprintf(stderr, "return barrier installation failed: %lu\n",
                         GetLastError());
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
    }
    const std::uint64_t continue_status = 3;
    if (!Write(process,
               remote_trigger + offsetof(TriggerContext, status),
               &continue_status, sizeof(continue_status))) {
        std::fprintf(stderr, "payload TLS boundary release failed: %lu\n",
                     GetLastError());
        restore_return_instruction();
        stop_owned_threads();
        CloseHandle(thread);
        CloseHandle(watcher_thread);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
    bool return_barrier_success = !return_barrier_enabled;
    if (return_barrier_enabled) {
        const auto target_exited = [&]() {
            return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        };
        const auto close_local_handles = [&]() {
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
        };
        ReturnCaptureState capture_state{};
        bool barrier_aborted = false;
        bool clean_target_exit = false;
        bool capture_failed = false;
        for (;;) {
            if (WaitForSingleObject(barrier_abort_event, 0) ==
                WAIT_OBJECT_0) {
                barrier_aborted = true;
                break;
            }
            if (target_exited()) {
                clean_target_exit = true;
                break;
            }
            SIZE_T capture_read = 0;
            if (!ReadProcessMemory(
                    process, remote_return_capture, &capture_state,
                    sizeof(capture_state), &capture_read) ||
                capture_read != sizeof(capture_state)) {
                clean_target_exit = target_exited();
                capture_failed = !clean_target_exit;
                break;
            }
            if (capture_state.marker == 1)
                break;
            if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
                capture_failed = true;
                break;
            }
            Sleep(1);
        }
        if (clean_target_exit) {
            close_local_handles();
            return 0;
        }
        const bool marker_valid =
            capture_state.marker == 1 &&
            capture_state.context_address != 0;
        const bool instruction_restored =
            restore_return_instruction();
        if (barrier_aborted || capture_failed || !marker_valid ||
            !instruction_restored) {
            std::fprintf(stderr,
                         "return barrier aborted=%u failed=%u marker=%u restored=%u\n",
                         barrier_aborted ? 1u : 0u,
                         capture_failed ? 1u : 0u,
                         marker_valid ? 1u : 0u,
                         instruction_restored ? 1u : 0u);
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        if (!SetEvent(barrier_ready_event)) {
            std::fprintf(stderr, "return barrier ready signal failed: %lu\n",
                         GetLastError());
            stop_owned_threads();
            CloseHandle(thread);
            CloseHandle(watcher_thread);
            CloseHandle(poll_thread);
            CloseHandle(process);
            return 1;
        }
        std::printf("return_barrier ready=1 marker=1 release=0 restored=1 context=0x%llX\n",
                    static_cast<unsigned long long>(
                        capture_state.context_address));
        HANDLE parked_waits[] = {process, barrier_abort_event};
        const DWORD parked_wait =
            WaitForMultipleObjects(2, parked_waits, FALSE, INFINITE);
        if (parked_wait == WAIT_OBJECT_0) {
            close_local_handles();
            return 0;
        }
        std::fprintf(stderr, "return barrier parked wait ended: %lu\n",
                     parked_wait);
        stop_owned_threads();
        CloseHandle(thread);
        CloseHandle(watcher_thread);
        CloseHandle(poll_thread);
        CloseHandle(process);
        return 1;
    }
#ifdef KIRKWARE_PARK_THREAD
    const ULONGLONG watcher_finish_deadline = GetTickCount64() + 30000;
    do {
        SIZE_T finish_read = 0;
        if (!ReadProcessMemory(process, remote_watcher, &watcher_context,
                               sizeof(watcher_context), &finish_read) ||
            finish_read != sizeof(watcher_context))
            break;
        if (watcher_context.status != 2)
            break;
        Sleep(1);
    } while (GetTickCount64() < watcher_finish_deadline);
    DWORD watcher_wait = WaitForSingleObject(watcher_thread, 0);
    const DWORD wait = WaitForSingleObject(thread, 0);
    const DWORD poll_wait = WaitForSingleObject(poll_thread, 0);
#else
    const DWORD wait = WaitForSingleObject(thread, 60000);
    DWORD watcher_wait = WaitForSingleObject(watcher_thread, 5000);
    const DWORD poll_wait = WaitForSingleObject(poll_thread, 5000);
#endif
#ifndef KIRKWARE_PARK_THREAD
    if (watcher_wait == WAIT_TIMEOUT) {
        TerminateThread(watcher_thread, 0);
        WaitForSingleObject(watcher_thread, 5000);
    }
#endif
    DWORD result = 0;
    DWORD watcher_result = 0;
    DWORD poll_result = 0;
    const bool trigger_exit_read = GetExitCodeThread(thread, &result) != FALSE;
    const bool watcher_exit_read =
        GetExitCodeThread(watcher_thread, &watcher_result) != FALSE;
    const bool poll_exit_read =
        GetExitCodeThread(poll_thread, &poll_result) != FALSE;
    SIZE_T trigger_read = 0;
    const bool trigger_state_read =
        ReadProcessMemory(process, remote_trigger, &trigger_context,
                          sizeof(trigger_context), &trigger_read) != FALSE &&
        trigger_read == sizeof(trigger_context);
    watcher_read = 0;
    const bool watcher_state_read =
        ReadProcessMemory(process, remote_watcher, &watcher_context,
                          sizeof(watcher_context), &watcher_read) != FALSE &&
        watcher_read == sizeof(watcher_context);
    SIZE_T poller_state_read = 0;
    poller_ready = 0;
    const bool poller_state_valid =
        ReadProcessMemory(process, remote_poller + 0x100,
                          &poller_ready, sizeof(poller_ready),
                          &poller_state_read) != FALSE &&
        poller_state_read == sizeof(poller_ready) && poller_ready == 1;
    if (fixed_unix_time) {
        std::uint64_t time_slot_after = 0;
        SIZE_T time_read = 0;
        ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kResolvedTimeSlotRva),
            &time_slot_after, sizeof(time_slot_after), &time_read);
        std::printf(
            "time_stub_post_entry slot=0x%llX value=0x%llX "
            "expected=0x%llX read=%llu\n",
            static_cast<unsigned long long>(
                image_base + kResolvedTimeSlotRva),
            static_cast<unsigned long long>(time_slot_after),
            static_cast<unsigned long long>(time_stub_address),
            static_cast<unsigned long long>(time_read));
    }
    std::uint32_t source_guard = 0;
    std::uint64_t sentinel = 0;
    std::uint64_t sentinel_links[2]{};
    MEMORY_BASIC_INFORMATION sentinel_memory{};
    SIZE_T sentinel_read = 0;
    const bool guard_valid =
        ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kSourceGuardRva),
            &source_guard, sizeof(source_guard), &sentinel_read) &&
        sentinel_read == sizeof(source_guard) && source_guard != 0 &&
        source_guard != 0xFFFFFFFFu &&
        (source_guard & 0x80000000u) != 0;
    const bool slot_valid =
        ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(image_base + kSentinelSlotRva),
            &sentinel, sizeof(sentinel), &sentinel_read) &&
        sentinel_read == sizeof(sentinel) && sentinel != 0 &&
        (sentinel & 7u) == 0 &&
        sentinel == watcher_context.observed_node;
    const bool memory_valid =
        slot_valid &&
        VirtualQueryEx(process, reinterpret_cast<const void*>(sentinel),
                       &sentinel_memory, sizeof(sentinel_memory)) ==
            sizeof(sentinel_memory) &&
        sentinel_memory.State == MEM_COMMIT &&
        (sentinel_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
    const bool links_valid =
        memory_valid &&
        ReadProcessMemory(process, reinterpret_cast<const void*>(sentinel),
                          sentinel_links, sizeof(sentinel_links),
                          &sentinel_read) &&
        sentinel_read == sizeof(sentinel_links) &&
        sentinel_links[0] == sentinel && sentinel_links[1] == sentinel;
    Write(process, reinterpret_cast<void*>(locale_global), &mbcinfo_address,
          sizeof(mbcinfo_address));
    std::printf("pid=%lu image=0x%llX blob=0x%llX context=0x%llX r8=0x%llX handler=0x%llX loader_managed=%u tls=%lu source_guard=0x%08X sentinel=0x%llX watcher_status=0x%llX trigger_status=0x%llX watcher_wait=%lu wait=%lu poll_wait=%lu watcher_result=0x%08lX result=0x%08lX poll_result=0x%08lX\n",
                pid, static_cast<unsigned long long>(image_base),
                static_cast<unsigned long long>(blob),
                static_cast<unsigned long long>(context_address),
                static_cast<unsigned long long>(entry_argument),
                static_cast<unsigned long long>(handler_address),
                loader_managed ? 1u : 0u, tls_index,
                source_guard,
                static_cast<unsigned long long>(sentinel),
                static_cast<unsigned long long>(watcher_context.status),
                static_cast<unsigned long long>(trigger_context.status),
                watcher_wait, wait, poll_wait, watcher_result, result,
                poll_result);
    const bool clean_shutdown_after_success =
        return_barrier_enabled &&
        WaitForSingleObject(barrier_success_event, 0) == WAIT_OBJECT_0 &&
        WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(thread);
    CloseHandle(watcher_thread);
    CloseHandle(poll_thread);
    CloseHandle(process);
#ifdef KIRKWARE_PARK_THREAD
    const bool trigger_lifetime_valid =
        wait == WAIT_TIMEOUT && trigger_exit_read && result == STILL_ACTIVE;
    const bool watcher_lifetime_valid =
        watcher_wait == WAIT_TIMEOUT && watcher_exit_read &&
        watcher_result == STILL_ACTIVE;
    const bool poller_lifetime_valid =
        poll_wait == WAIT_TIMEOUT && poll_exit_read &&
        poll_result == STILL_ACTIVE;
#else
    const bool trigger_lifetime_valid =
        wait == WAIT_OBJECT_0 && trigger_exit_read;
    const bool watcher_lifetime_valid =
        watcher_wait == WAIT_OBJECT_0 && watcher_exit_read &&
        watcher_result != 0;
    const bool poller_lifetime_valid =
        poll_wait == WAIT_OBJECT_0 && poll_exit_read;
#endif
    return clean_shutdown_after_success ||
           (return_barrier_success && trigger_lifetime_valid &&
                   trigger_state_read && trigger_context.status == 1 &&
                   watcher_lifetime_valid &&
                   watcher_state_read && watcher_context.status == 1 &&
                   poller_lifetime_valid && poller_state_valid &&
                   guard_valid && links_valid)
               ? 0
               : 1;
}
