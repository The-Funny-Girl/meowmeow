#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include "kirkware_hook_fingerprints.hpp"
#include "kirkware_hook_normalized.hpp"
#include "kirkware_deferred_event_worker.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct KirkwareGameHookInterfaces {
    std::uint64_t client = 0;
    std::uint64_t model = 0;
    std::uint64_t lua_shared = 0;
    std::uint64_t panel = 0;
};

int kirkware_carrier_entry(int, char**);

namespace {

constexpr std::uint64_t kDefaultImageBase = 0x000001E5DCC00000ull;
constexpr std::uint64_t kActiveImageSize = 0x1162000;
constexpr std::uint32_t kInitializeHookRva = 0x300360;
constexpr std::uint32_t kCreateHookRva = 0x3003F0;
constexpr std::uint32_t kRemoveHookRva = 0x3006D0;
constexpr std::uint32_t kEnableHookRva = 0x300870;
constexpr std::size_t kDetourPrefixSize = 16;
constexpr std::size_t kTargetPrefixSize = 5;
constexpr std::size_t kMinHookBlockSize = 0x40;
constexpr std::size_t kClMoveHookIndex = 50;
constexpr std::size_t kNetChannelProcessPacketHookIndex = 51;
constexpr std::size_t kNetChannelSendNetMsgHookIndex = 52;
constexpr std::size_t kNetChannelSendDatagramHookIndex = 53;
constexpr std::size_t kBreakLcHookIndex = 54;
constexpr std::size_t kLuaRunStringHookIndex = 55;
constexpr std::size_t kLuaFilterAdvanceHookIndex = 56;
constexpr std::size_t kLuaFilterAdvanceIntegerHookIndex = 57;
constexpr std::size_t kLuaVmGuardHookIndex = 58;
constexpr std::size_t kBoneBuildTransformationsHookIndex = 59;
constexpr std::size_t kTransitionShutdownHookIndex = 60;
constexpr std::size_t kMotionBlurHookIndex = 61;
constexpr std::size_t kHostFilterTimeHookIndex = 62;
constexpr std::size_t kCamThinkHookIndex = 63;
constexpr std::size_t kClientLuaTeardownHookIndex = 64;
constexpr std::size_t kLocalShouldInterpolateHookIndex = 65;
constexpr std::size_t kClientEventHookIndex = 66;

constexpr std::array<std::uint32_t, 11> kSteamFriendsMethodOffsets{{
    0x000, 0x008, 0x038, 0x050, 0x0E8, 0x120,
    0x128, 0x158, 0x160, 0x168, 0x188,
}};
constexpr std::array<std::uint32_t, 1> kSteamUserMethodOffsets{{0x010}};
constexpr std::array<std::uint32_t, 3> kSteamUserStatsMethodOffsets{{
    0x000, 0x040, 0x050,
}};
constexpr std::array<std::uint32_t, 2> kSteamUtilsMethodOffsets{{
    0x028, 0x030,
}};

struct SteamInterfaceSpec {
    const char* accessor;
    const char* legacy_version;
    std::uint32_t slot_rva;
    const std::uint32_t* method_offsets;
    std::size_t method_count;
};

constexpr std::array<SteamInterfaceSpec, 4> kSteamInterfaces{{
    {nullptr, "SteamFriends017", 0xB6BCF0,
     kSteamFriendsMethodOffsets.data(), kSteamFriendsMethodOffsets.size()},
    {"SteamAPI_SteamUser_v023", nullptr, 0xB6BCF8,
     kSteamUserMethodOffsets.data(), kSteamUserMethodOffsets.size()},
    {nullptr, "STEAMUSERSTATS_INTERFACE_VERSION011", 0xB6BD00,
     kSteamUserStatsMethodOffsets.data(),
     kSteamUserStatsMethodOffsets.size()},
    {"SteamAPI_SteamUtils_v010", nullptr, 0xB6BD58,
     kSteamUtilsMethodOffsets.data(), kSteamUtilsMethodOffsets.size()},
}};

class Handle {
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    explicit operator bool() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_ = nullptr;
};

struct ModuleInfo {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::string path;
};

struct Failure {
    const char* stage = "none";
    const char* predicate = "none";
    std::size_t index = 0;
    std::uint64_t address = 0;
    std::uint64_t observed = 0;
    std::uint64_t expected = 0;
    DWORD error = ERROR_SUCCESS;
};

bool Fail(Failure& failure, const char* stage, const char* predicate,
          std::size_t index = 0, std::uint64_t address = 0,
          std::uint64_t observed = 0, std::uint64_t expected = 0,
          DWORD error = ERROR_SUCCESS) {
    failure.stage = stage;
    failure.predicate = predicate;
    failure.index = index;
    failure.address = address;
    failure.observed = observed;
    failure.expected = expected;
    failure.error = error;
    return false;
}

void PrintFailure(const Failure& failure) {
    std::fprintf(
        stderr,
        "kirkware hook install failed stage=%s predicate=%s index=%llu "
        "address=0x%016llX observed=0x%016llX expected=0x%016llX error=%lu\n",
        failure.stage, failure.predicate,
        static_cast<unsigned long long>(failure.index),
        static_cast<unsigned long long>(failure.address),
        static_cast<unsigned long long>(failure.observed),
        static_cast<unsigned long long>(failure.expected),
        static_cast<unsigned long>(failure.error));
    std::fflush(stderr);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string BaseName(const std::string& path) {
    const auto separator = path.find_last_of("\\/");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

ModuleInfo FindModule(DWORD pid, const char* requested) {
    const std::string wanted = Lower(requested);
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snapshot) {
            if (GetLastError() == ERROR_BAD_LENGTH) {
                Sleep(2);
                continue;
            }
            return {};
        }
        MODULEENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Module32First(snapshot.Get(), &entry)) {
            do {
                if (Lower(entry.szModule) == wanted ||
                    Lower(BaseName(entry.szExePath)) == wanted) {
                    return {
                        reinterpret_cast<std::uint64_t>(entry.modBaseAddr),
                        static_cast<std::uint64_t>(entry.modBaseSize),
                        entry.szExePath};
                }
            } while (Module32Next(snapshot.Get(), &entry));
        }
        if (GetLastError() != ERROR_BAD_LENGTH)
            return {};
        Sleep(2);
    }
    return {};
}

std::uint64_t ExportRva(const ModuleInfo& module, const char* symbol) {
    if (!module.base || !module.size || module.path.empty())
        return 0;
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return 0;
    const FARPROC address = GetProcAddress(local, symbol);
    const std::uint64_t rva =
        address ? reinterpret_cast<std::uint64_t>(address) -
                      reinterpret_cast<std::uint64_t>(local)
                : 0;
    FreeLibrary(local);
    return rva < module.size ? rva : 0;
}

bool ReadExact(HANDLE process, std::uint64_t address, void* output,
               std::size_t size) {
    SIZE_T read = 0;
    return process && address && output && size &&
           ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             output, size, &read) != FALSE &&
           read == size;
}

template <typename T>
bool ReadValue(HANDLE process, std::uint64_t address, T& output) {
    return ReadExact(process, address, &output, sizeof(output));
}

bool WriteExact(HANDLE process, std::uint64_t address, const void* input,
                std::size_t size) {
    SIZE_T written = 0;
    return process && address && input && size &&
           WriteProcessMemory(process, reinterpret_cast<void*>(address), input,
                              size, &written) != FALSE &&
           written == size;
}

bool RangeInModule(std::uint64_t address, std::size_t size,
                   const ModuleInfo& module) {
    return module.base && module.size && address >= module.base &&
           size <= module.size && address - module.base <= module.size - size;
}

bool RemoteRange(HANDLE process, std::uint64_t address, std::size_t size,
                 bool executable) {
    if (!process || !address || !size || address > UINT64_MAX - size)
        return false;
    std::uint64_t cursor = address;
    const std::uint64_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD access = memory.Protect & 0xFFu;
        const bool readable =
            access == PAGE_READONLY || access == PAGE_READWRITE ||
            access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
            access == PAGE_EXECUTE_READWRITE ||
            access == PAGE_EXECUTE_WRITECOPY;
        const bool runnable =
            access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
            access == PAGE_EXECUTE_READWRITE ||
            access == PAGE_EXECUTE_WRITECOPY;
        if (executable ? !runnable : !readable)
            return false;
        const auto region =
            reinterpret_cast<std::uint64_t>(memory.BaseAddress);
        if (region > cursor || memory.RegionSize > UINT64_MAX - region)
            return false;
        const std::uint64_t region_end = region + memory.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = std::min(region_end, end);
    }
    return true;
}

bool ExecutableRange(HANDLE process, std::uint64_t address,
                     std::size_t size = 1) {
    return RemoteRange(process, address, size, true);
}

bool ReadableRange(HANDLE process, std::uint64_t address,
                   std::size_t size = 1) {
    return RemoteRange(process, address, size, false);
}

bool WritableRange(HANDLE process, std::uint64_t address,
                   std::size_t size = 1) {
    if (!process || !address || !size || address > UINT64_MAX - size)
        return false;
    std::uint64_t cursor = address;
    const std::uint64_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD access = memory.Protect & 0xFFu;
        if (access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READWRITE &&
            access != PAGE_EXECUTE_WRITECOPY)
            return false;
        const auto region =
            reinterpret_cast<std::uint64_t>(memory.BaseAddress);
        if (region > cursor || memory.RegionSize > UINT64_MAX - region)
            return false;
        const std::uint64_t region_end = region + memory.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = std::min(region_end, end);
    }
    return true;
}

bool WriteProtectedQword(HANDLE process, std::uint64_t address,
                         std::uint64_t value) {
    DWORD previous = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address),
                          sizeof(value), PAGE_READWRITE, &previous))
        return false;
    const bool wrote = WriteExact(process, address, &value, sizeof(value));
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(address),
                         sizeof(value), previous, &ignored) != FALSE;
    std::uint64_t observed = 0;
    return wrote && restored && ReadValue(process, address, observed) &&
           observed == value;
}

std::uint64_t PrefixValue(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t value = 0;
    if (bytes && size)
        std::memcpy(&value, bytes, std::min(size, sizeof(value)));
    return value;
}

struct RemoteCallContext {
    std::uint64_t function;
    std::uint64_t argument1;
    std::uint64_t argument2;
    std::uint64_t argument3;
    std::uint64_t result;
    std::uint32_t completed;
};

static_assert(offsetof(RemoteCallContext, completed) == 0x28);

thread_local bool* g_remote_call_rollback_safe = nullptr;

class RemoteCallSafetyScope {
public:
    explicit RemoteCallSafetyScope(bool& rollback_safe)
        : previous_(g_remote_call_rollback_safe) {
        g_remote_call_rollback_safe = &rollback_safe;
    }

    ~RemoteCallSafetyScope() {
        g_remote_call_rollback_safe = previous_;
    }

    RemoteCallSafetyScope(const RemoteCallSafetyScope&) = delete;
    RemoteCallSafetyScope& operator=(const RemoteCallSafetyScope&) = delete;

private:
    bool* previous_;
};

bool RemoteCallsRollbackSafe() {
    return !g_remote_call_rollback_safe || *g_remote_call_rollback_safe;
}

void MarkRemoteCallsNonQuiescent() {
    if (g_remote_call_rollback_safe)
        *g_remote_call_rollback_safe = false;
}

bool RemoteCall3(HANDLE process, std::uint64_t function,
                 std::uint64_t argument1, std::uint64_t argument2,
                 std::uint64_t argument3, std::uint64_t& result,
                 Failure& failure, const char* stage, std::size_t index,
                 bool* call_completed = nullptr) {
    static constexpr std::array<std::uint8_t, 39> stub{
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8B, 0x53, 0x10,
        0x4C, 0x8B, 0x43, 0x18, 0xFF, 0x13,
        0x48, 0x89, 0x43, 0x20, 0xC7, 0x43, 0x28, 0x01,
        0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B,
        0xC3};
    if (call_completed)
        *call_completed = false;
    if (!ExecutableRange(process, function, 16))
        return Fail(failure, stage, "function_executable", index, function);
    RemoteCallContext context{
        function, argument1, argument2, argument3, UINT64_MAX, 0};
    void* remote_context = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* remote_code = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_context || !remote_code) {
        const DWORD error = GetLastError();
        if (remote_context)
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        if (remote_code)
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_allocate", index, 0, 0, 0,
                    error);
    }
    if (!WriteExact(process,
                    reinterpret_cast<std::uint64_t>(remote_context),
                    &context, sizeof(context)) ||
        !WriteExact(process, reinterpret_cast<std::uint64_t>(remote_code),
                    stub.data(), stub.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_write", index, 0, 0, 0, error);
    }
    DWORD previous = 0;
    if (!VirtualProtectEx(process, remote_code, 0x1000, PAGE_EXECUTE_READ,
                          &previous) ||
        !FlushInstructionCache(process, remote_code, stub.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_code_prepare", index,
                    reinterpret_cast<std::uint64_t>(remote_code), 0, 0,
                    error);
    }
    Handle thread(CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_code), remote_context,
        0, nullptr));
    if (!thread) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_thread_create", index,
                    function, 0, 0, error);
    }
    const DWORD wait = WaitForSingleObject(thread.Get(), 30000);
    if (wait != WAIT_OBJECT_0) {
        MarkRemoteCallsNonQuiescent();
        return Fail(failure, stage, "remote_thread_wait", index, function,
                    wait, WAIT_OBJECT_0,
                    wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }
    if (!ReadValue(process,
                   reinterpret_cast<std::uint64_t>(remote_context),
                   context)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_result_read", index,
                    reinterpret_cast<std::uint64_t>(remote_context), 0, 0,
                    error);
    }
    if (context.completed != 1) {
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_completed", index, function,
                    context.completed, 1);
    }
    if (call_completed)
        *call_completed = true;
    result = context.result;
    const bool freed_context =
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE) != FALSE;
    const bool freed_code =
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE) != FALSE;
    if (!freed_context || !freed_code)
        return Fail(failure, stage, "remote_free", index, 0,
                    freed_context && freed_code, 1, GetLastError());
    return true;
}

struct RemoteCall4Context {
    std::uint64_t function;
    std::uint64_t argument1;
    std::uint64_t argument2;
    std::uint64_t argument3;
    std::uint64_t argument4;
    std::uint64_t result;
    std::uint32_t completed;
};

bool RemoteCall4(HANDLE process, std::uint64_t function,
                 std::uint64_t argument1, std::uint64_t argument2,
                 std::uint64_t argument3, std::uint64_t argument4,
                 std::uint64_t& result, Failure& failure, const char* stage,
                 std::size_t index, bool* call_completed = nullptr) {
    static constexpr std::array<std::uint8_t, 43> stub{
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8B, 0x53, 0x10,
        0x4C, 0x8B, 0x43, 0x18, 0x4C, 0x8B, 0x4B, 0x20,
        0xFF, 0x13, 0x48, 0x89, 0x43, 0x28, 0xC7, 0x43,
        0x30, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
        0x20, 0x5B, 0xC3};
    if (call_completed)
        *call_completed = false;
    if (!ExecutableRange(process, function, 16))
        return Fail(failure, stage, "function_executable", index, function);
    RemoteCall4Context context{
        function, argument1, argument2, argument3, argument4, UINT64_MAX, 0};
    void* remote_context = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* remote_code = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_context || !remote_code) {
        const DWORD error = GetLastError();
        if (remote_context)
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        if (remote_code)
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_allocate", index, 0, 0, 0,
                    error);
    }
    if (!WriteExact(process,
                    reinterpret_cast<std::uint64_t>(remote_context),
                    &context, sizeof(context)) ||
        !WriteExact(process, reinterpret_cast<std::uint64_t>(remote_code),
                    stub.data(), stub.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_write", index, 0, 0, 0, error);
    }
    DWORD previous = 0;
    if (!VirtualProtectEx(process, remote_code, 0x1000, PAGE_EXECUTE_READ,
                          &previous) ||
        !FlushInstructionCache(process, remote_code, stub.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_code_prepare", index,
                    reinterpret_cast<std::uint64_t>(remote_code), 0, 0,
                    error);
    }
    Handle thread(CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_code), remote_context,
        0, nullptr));
    if (!thread) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_thread_create", index,
                    function, 0, 0, error);
    }
    const DWORD wait = WaitForSingleObject(thread.Get(), 30000);
    if (wait != WAIT_OBJECT_0) {
        MarkRemoteCallsNonQuiescent();
        return Fail(failure, stage, "remote_thread_wait", index, function,
                    wait, WAIT_OBJECT_0,
                    wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }
    if (!ReadValue(process,
                   reinterpret_cast<std::uint64_t>(remote_context),
                   context)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_result_read", index,
                    reinterpret_cast<std::uint64_t>(remote_context), 0, 0,
                    error);
    }
    if (context.completed != 1) {
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, stage, "remote_completed", index, function,
                    context.completed, 1);
    }
    if (call_completed)
        *call_completed = true;
    result = context.result;
    const bool freed_context =
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE) != FALSE;
    const bool freed_code =
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE) != FALSE;
    if (!freed_context || !freed_code)
        return Fail(failure, stage, "remote_free", index, 0,
                    freed_context && freed_code, 1, GetLastError());
    return true;
}

bool ValidateImage(HANDLE process, std::uint64_t image_base,
                   Failure& failure) {
    if (!image_base || image_base > UINT64_MAX - kActiveImageSize)
        return Fail(failure, "image", "allocation_range", 0, image_base,
                    image_base, kActiveImageSize);
    std::uint64_t cursor = image_base;
    const std::uint64_t end = image_base + kActiveImageSize;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &memory, sizeof(memory)) != sizeof(memory))
            return Fail(failure, "image", "allocation_query", 0, cursor,
                        0, 1, GetLastError());
        const auto region =
            reinterpret_cast<std::uint64_t>(memory.BaseAddress);
        const auto allocation =
            reinterpret_cast<std::uint64_t>(memory.AllocationBase);
        if (memory.State != MEM_COMMIT || allocation != image_base ||
            region > cursor || memory.RegionSize > UINT64_MAX - region ||
            region + memory.RegionSize <= cursor)
            return Fail(failure, "image", "allocation_committed", 0,
                        cursor, allocation, image_base);
        cursor = std::min<std::uint64_t>(region + memory.RegionSize, end);
    }
    IMAGE_DOS_HEADER dos{};
    if (!ReadValue(process, image_base, dos))
        return Fail(failure, "image", "dos_read", 0, image_base, 0, 0,
                    GetLastError());
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x1000 - static_cast<LONG>(sizeof(IMAGE_NT_HEADERS64)))
        return Fail(failure, "image", "dos_exact", 0, image_base,
                    dos.e_magic, IMAGE_DOS_SIGNATURE);
    IMAGE_NT_HEADERS64 nt{};
    const std::uint64_t nt_address =
        image_base + static_cast<std::uint32_t>(dos.e_lfanew);
    if (!ReadValue(process, nt_address, nt))
        return Fail(failure, "image", "nt_read", 0, nt_address, 0, 0,
                    GetLastError());
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != 0 ||
        nt.FileHeader.NumberOfSections != 8 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.ImageBase != 0 ||
        nt.OptionalHeader.SizeOfImage != 0 ||
        nt.OptionalHeader.SizeOfHeaders != 0x400)
        return Fail(failure, "image", "nt_exact", 0, nt_address,
                    nt.OptionalHeader.SizeOfImage, kActiveImageSize);
    static constexpr std::array<std::uint8_t, 16> minhook_prefix{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
    static constexpr std::array<std::uint8_t, 16> initialize_prefix{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33};
    std::array<std::uint8_t, initialize_prefix.size()> initialize_observed{};
    const std::uint64_t initialize_address =
        image_base + kInitializeHookRva;
    if (!ExecutableRange(process, initialize_address,
                         initialize_observed.size()) ||
        !ReadExact(process, initialize_address, initialize_observed.data(),
                   initialize_observed.size()))
        return Fail(failure, "image", "minhook_initialize_read",
                    kInitializeHookRva, initialize_address, 0, 0,
                    GetLastError());
    if (initialize_observed != initialize_prefix)
        return Fail(
            failure, "image", "minhook_initialize_prefix",
            kInitializeHookRva, initialize_address,
            PrefixValue(initialize_observed.data(), initialize_observed.size()),
            PrefixValue(initialize_prefix.data(), initialize_prefix.size()));
    for (const std::uint32_t rva :
         {kCreateHookRva, kRemoveHookRva, kEnableHookRva}) {
        std::array<std::uint8_t, minhook_prefix.size()> observed{};
        const std::uint64_t address = image_base + rva;
        if (!ExecutableRange(process, address, observed.size()) ||
            !ReadExact(process, address, observed.data(), observed.size()))
            return Fail(failure, "image", "minhook_entry_read", rva,
                        address, 0, 0, GetLastError());
        if (observed != minhook_prefix)
            return Fail(failure, "image", "minhook_entry_prefix", rva,
                        address, PrefixValue(observed.data(), observed.size()),
                        PrefixValue(minhook_prefix.data(),
                                    minhook_prefix.size()));
    }
    return true;
}

struct InterfaceRequest {
    const ModuleInfo* module;
    const char* name;
    std::uint32_t slot_rva;
    std::uint64_t result = 0;
    std::uint64_t previous = 0;
    bool changed = false;
};

bool ValidateInterfaceResult(HANDLE process, InterfaceRequest& request,
                             std::size_t index, Failure& failure) {
    std::uint64_t vtable = 0;
    if (!request.result ||
        !ReadableRange(process, request.result, sizeof(vtable)) ||
        !ReadValue(process, request.result, vtable) ||
        !RangeInModule(vtable, sizeof(std::uint64_t), *request.module))
        return Fail(failure, "interface", "result_exact", index,
                    request.result, vtable, request.module->base);
    return true;
}

bool ResolveInterface(HANDLE process, InterfaceRequest& request,
                      std::size_t index,
                      Failure& failure) {
    const std::uint64_t factory_rva =
        ExportRva(*request.module, "CreateInterface");
    if (!factory_rva)
        return Fail(failure, "interface", "factory_rva", index,
                    request.module->base, 0, 1);
    const std::size_t name_size = std::strlen(request.name) + 1;
    void* remote_name = VirtualAllocEx(
        process, nullptr, name_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_name)
        return Fail(failure, "interface", "name_allocate", index, 0, 0, 0,
                    GetLastError());
    if (!WriteExact(process, reinterpret_cast<std::uint64_t>(remote_name),
                    request.name, name_size)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_name, 0, MEM_RELEASE);
        return Fail(failure, "interface", "name_write", index,
                    reinterpret_cast<std::uint64_t>(remote_name), 0, 0,
                    error);
    }
    std::uint64_t result = 0;
    const bool called = RemoteCall3(
        process, request.module->base + factory_rva,
        reinterpret_cast<std::uint64_t>(remote_name), 0, 0, result, failure,
        "interface_call", index);
    const bool quiescent = RemoteCallsRollbackSafe();
    const bool freed = quiescent &&
        VirtualFreeEx(process, remote_name, 0, MEM_RELEASE) != FALSE;
    if (!called)
        return false;
    if (!freed)
        return Fail(failure, "interface", "name_free", index,
                    reinterpret_cast<std::uint64_t>(remote_name), 0, 1,
                    GetLastError());
    request.result = result;
    return ValidateInterfaceResult(process, request, index, failure);
}

struct SteamInterfaceState {
    std::uint64_t accessor = 0;
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t published_object = 0;
    std::uint64_t previous = 0;
    bool changed = false;
};

bool ValidateSteamInterfaceObject(
    HANDLE process, const SteamInterfaceSpec& spec,
    SteamInterfaceState& state, std::size_t index, Failure& failure,
    const char* stage) {
    if (!state.object ||
        !ReadableRange(process, state.object, 8))
        return Fail(failure, stage, "object_readable", index,
                    state.object, 0, 1, GetLastError());

    std::uint64_t vtable = 0;
    if (!ReadValue(process, state.object, vtable) || !vtable)
        return Fail(failure, stage, "vtable_read", index, state.object,
                    vtable, 1, GetLastError());
    std::uint32_t maximum_offset = 0;
    for (std::size_t method = 0; method != spec.method_count; ++method)
        maximum_offset = std::max(maximum_offset,
                                  spec.method_offsets[method]);
    if (!ReadableRange(process, vtable,
                       static_cast<std::size_t>(maximum_offset) + 8))
        return Fail(failure, stage, "vtable_readable", index, vtable,
                    maximum_offset, 1, GetLastError());
    for (std::size_t method = 0; method != spec.method_count; ++method) {
        const std::uint64_t method_slot =
            vtable + spec.method_offsets[method];
        std::uint64_t target = 0;
        if (!ReadValue(process, method_slot, target) || !target ||
            !ExecutableRange(process, target, 16))
            return Fail(failure, stage, "method_executable", method,
                        method_slot, target, 1, GetLastError());
    }

    state.vtable = vtable;
    state.published_object = state.object;
    return true;
}

bool PrepareSteamInterface(HANDLE process, std::uint64_t image_base,
                           const ModuleInfo& steam_api,
                           const SteamInterfaceSpec& spec,
                           SteamInterfaceState& state, std::size_t index,
                           Failure& failure) {
    std::uint64_t result = 0;
    if (spec.legacy_version) {
        const std::uint64_t user_rva =
            ExportRva(steam_api, "SteamAPI_GetHSteamUser");
        const std::uint64_t find_rva = ExportRva(
            steam_api, "SteamInternal_FindOrCreateUserInterface");
        const std::uint64_t user_accessor = steam_api.base + user_rva;
        state.accessor = steam_api.base + find_rva;
        if (!user_rva || !find_rva ||
            !RangeInModule(user_accessor, 7, steam_api) ||
            !RangeInModule(state.accessor, 16, steam_api) ||
            !ExecutableRange(process, user_accessor, 7) ||
            !ExecutableRange(process, state.accessor, 16))
            return Fail(failure, "steam_legacy", "exports_executable",
                        index, state.accessor, find_rva, steam_api.base,
                        GetLastError());
        std::uint64_t user = 0;
        if (!RemoteCall3(process, user_accessor, 0, 0, 0, user, failure,
                         "steam_user_handle", index))
            return false;
        if (static_cast<std::uint32_t>(user) == 0)
            return Fail(failure, "steam_legacy", "user_nonzero", index,
                        user_accessor, user, 1);
        const std::size_t version_size =
            std::strlen(spec.legacy_version) + 1;
        void* remote_version = VirtualAllocEx(
            process, nullptr, version_size, MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!remote_version ||
            !WriteExact(process,
                        reinterpret_cast<std::uint64_t>(remote_version),
                        spec.legacy_version, version_size)) {
            const DWORD error = GetLastError();
            if (remote_version)
                VirtualFreeEx(process, remote_version, 0, MEM_RELEASE);
            return Fail(failure, "steam_legacy", "version_write", index,
                        reinterpret_cast<std::uint64_t>(remote_version), 0,
                        version_size, error);
        }
        const bool called = RemoteCall3(
            process, state.accessor, static_cast<std::uint32_t>(user),
            reinterpret_cast<std::uint64_t>(remote_version), 0, result,
            failure, "steam_legacy_call", index);
        const bool quiescent = RemoteCallsRollbackSafe();
        const bool freed = quiescent &&
            VirtualFreeEx(process, remote_version, 0, MEM_RELEASE) != FALSE;
        if (!called)
            return false;
        if (!freed)
            return Fail(failure, "steam_legacy", "version_free", index,
                        reinterpret_cast<std::uint64_t>(remote_version), 0,
                        1, GetLastError());
    } else {
        const std::uint64_t accessor_rva =
            ExportRva(steam_api, spec.accessor);
        if (!accessor_rva)
            return Fail(failure, "steam_accessor", "export_rva", index,
                        steam_api.base, 0, 1);
        state.accessor = steam_api.base + accessor_rva;
        if (!RangeInModule(state.accessor, 16, steam_api) ||
            !ExecutableRange(process, state.accessor, 16))
            return Fail(failure, "steam_accessor", "export_executable",
                        index, state.accessor, 0, steam_api.base,
                        GetLastError());
        if (!RemoteCall3(process, state.accessor, 0, 0, 0, result, failure,
                         "steam_accessor_call", index))
            return false;
    }
    state.object = result;
    if (!ValidateSteamInterfaceObject(process, spec, state, index, failure,
                                      "steam_interface"))
        return false;

    const std::uint64_t slot = image_base + spec.slot_rva;
    if (!ReadValue(process, slot, state.previous))
        return Fail(failure, "steam_slot", "original_read", index, slot,
                    0, 0, GetLastError());
    if (state.previous != 0 && state.previous != state.object)
        return Fail(failure, "steam_slot", "zero_or_same", index, slot,
                    state.previous, state.object);
    return true;
}

bool PublishSteamInterface(HANDLE process, std::uint64_t image_base,
                           const SteamInterfaceSpec& spec,
                           SteamInterfaceState& state, std::size_t index,
                           Failure& failure) {
    const std::uint64_t published = state.published_object;
    if (state.previous != published) {
        state.changed = true;
        if (!WriteProtectedQword(process, image_base + spec.slot_rva,
                                 published))
            return Fail(failure, "steam_slot", "publish", index,
                        image_base + spec.slot_rva, 0, published,
                        GetLastError());
    }
    std::uint64_t observed = 0;
    if (!ReadValue(process, image_base + spec.slot_rva, observed) ||
        observed != published)
        return Fail(failure, "steam_slot", "published_exact", index,
                    image_base + spec.slot_rva, observed, published,
                    GetLastError());
    return ValidateSteamInterfaceObject(process, spec, state, index,
                                        failure, "steam_post");
}

bool RollbackSteamInterfaces(
    HANDLE process, std::uint64_t image_base,
    std::array<SteamInterfaceState, kSteamInterfaces.size()>& states) {
    bool ok = true;
    for (std::size_t reverse = states.size(); reverse != 0; --reverse) {
        const std::size_t index = reverse - 1;
        auto& state = states[index];
        if (state.changed) {
            if (!WriteProtectedQword(
                    process,
                    image_base + kSteamInterfaces[index].slot_rva,
                    state.previous))
                ok = false;
            else
                state.changed = false;
        }
    }
    return ok;
}

enum class HookModule : std::uint8_t {
    Client,
    Engine,
    LuaShared,
    Vgui2,
};

struct DirectHookSpec {
    std::uint32_t object_slot_rva;
    std::uint32_t vtable_index;
    std::uint32_t detour_rva;
    std::uint32_t saved_slot_rva;
    HookModule module;
    std::uint32_t baseline_original_rva;
    const std::uint8_t* original_prefix;
    std::size_t original_prefix_size;
    const std::uint8_t* detour_prefix;
};

constexpr std::uint8_t kClientOriginalPrefix[]{
    0x48, 0x83, 0xEC, 0x28, 0x89, 0x15};
constexpr std::uint8_t kEngineOriginalFeec0Prefix[]{
    0x40, 0x55, 0x53, 0x56, 0x41, 0x54,
    0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kLuaOriginal13440Prefix[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xDA};
constexpr std::uint8_t kLuaOriginal13140Prefix[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02};
constexpr std::uint8_t kVguiOriginalPrefix[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01};
constexpr std::uint8_t kDetour31f8c0[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48};
constexpr std::uint8_t kDetour336b80[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x70, 0x0F, 0x29, 0x74, 0x24, 0x60, 0x48};
constexpr std::uint8_t kDetour31fc20[]{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C,
    0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48};
constexpr std::uint8_t kDetour31e4b0[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x30, 0x8B, 0xDA, 0x48, 0x8B, 0x05, 0xC5};
constexpr std::uint8_t kDetour31e460[]{
    0x48, 0x83, 0xEC, 0x38, 0x48, 0x3B, 0x15, 0x05,
    0xC4, 0x4C, 0x00, 0x75, 0x28, 0x33, 0xC0, 0x48};
constexpr std::uint8_t kLuaCloseResetExact[]{
    0x33, 0xC0, 0x48, 0x89, 0x05, 0x3A, 0xFE, 0x51,
    0x00, 0x48, 0x89, 0x05, 0xF3, 0xC3, 0x4C, 0x00,
    0x88, 0x05, 0x8A, 0xC1, 0x4C, 0x00, 0x88, 0x05,
    0xDC, 0xC0, 0x4C, 0x00, 0x88, 0x05, 0xC7, 0xC1,
    0x4C, 0x00, 0x88, 0x05, 0x77, 0xC1, 0x4C, 0x00};
constexpr std::uint8_t kDetour31e550[]{
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44,
    0x88, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08, 0x53};
constexpr std::uint8_t kDetour3359d0[]{
    0x44, 0x8B, 0xC2, 0xB8, 0xB7, 0x60, 0x0B, 0xB6,
    0xF7, 0xEA, 0x41, 0x03, 0xD0, 0xC1, 0xFA, 0x06};

constexpr std::array<DirectHookSpec, 6> kDirectHooks{{
    {0x7EA810, 35, 0x31F8C0, 0x80AA70, HookModule::Client,
     0x203C20, kClientOriginalPrefix, sizeof(kClientOriginalPrefix),
     kDetour31f8c0},
    {0x7EA8A8, 95, 0x336B80, 0x80AA28, HookModule::Engine,
     0x797A0, nullptr, 0, kDetour336b80},
    {0x7EA830, 20, 0x31FC20, 0x823850, HookModule::Engine,
     0xFF1C0, kEngineOriginalFeec0Prefix,
     sizeof(kEngineOriginalFeec0Prefix), kDetour31fc20},
    {0x7EA8C0, 4, 0x31E4B0, 0x80AA88, HookModule::LuaShared,
     0x134A0, kLuaOriginal13440Prefix, sizeof(kLuaOriginal13440Prefix),
     kDetour31e4b0},
    {0x7EA8C0, 5, 0x31E460, 0x80A9F8, HookModule::LuaShared,
     0x131A0, kLuaOriginal13140Prefix, sizeof(kLuaOriginal13140Prefix),
     kDetour31e460},
    {0x7EA858, 41, 0x31E550, 0x823878, HookModule::Vgui2,
     0x1C960, kVguiOriginalPrefix, sizeof(kVguiOriginalPrefix),
     kDetour31e550},
}};

enum class CloneInstallForm : std::uint8_t {
    Generic,
    Static,
};

struct CloneHookSpec {
    std::uint32_t object_slot_rva;
    std::uint32_t vtable_index;
    std::uint32_t detour_rva;
    std::uint32_t saved_slot_rva;
    HookModule module;
    CloneInstallForm form;
    std::uint32_t owner_rva;
    std::uint32_t owner_vtable_rva;
    std::uint32_t installer_rva;
    const std::uint8_t* detour_prefix;
};

constexpr std::uint8_t kCloneConstructorPrefix[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
constexpr std::uint8_t kCloneAllocatorPrefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xEB, 0x0F, 0x48, 0x8B, 0xCB, 0xE8, 0xA9};
constexpr std::uint8_t kCloneCleanupPrefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0x59, 0x08, 0x48, 0x85, 0xDB, 0x74, 0x25, 0x48};
constexpr std::uint8_t kInstaller2f4340Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xB9, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x91};
constexpr std::uint8_t kInstaller2ff160Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xB9, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x71};
constexpr std::uint8_t kInstaller31b300Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xB9, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD1};
constexpr std::uint8_t kInstaller31b0d0Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE9, 0xE9, 0xAF, 0xB8, 0x00, 0x50, 0x46};
constexpr std::uint8_t kInstaller31b210Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE9, 0x4D, 0xC9, 0xBB, 0x00, 0x50, 0x09};
constexpr std::uint8_t kInstaller3010c0Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE9, 0xBF, 0x45, 0xBA, 0x00, 0x50, 0x18};
constexpr std::uint8_t kInstaller31c430Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE9, 0xAD, 0xC2, 0xB7, 0x00, 0x50, 0x17};
constexpr std::uint8_t kInstaller349ad0Prefix[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE9, 0xE5, 0xCB, 0xDD, 0x00, 0x50, 0x6D};
constexpr std::uint8_t kDetour31ac30[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C};
constexpr std::uint8_t kDetour31b1a0[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint8_t kDetour31b280[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xDA, 0xFF, 0x15, 0xB1, 0x07, 0x85, 0x00, 0x48};
constexpr std::uint8_t kDetour300d80[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41};
constexpr std::uint8_t kDetour2f3ec0[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
constexpr std::uint8_t kDetour31bd50[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
constexpr std::uint8_t kDetour2fde50[]{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
    0x89, 0x70, 0x20, 0x48, 0x89, 0x48, 0x08, 0x57};
constexpr std::uint8_t kDetour349aa0[]{
    0x48, 0x83, 0xEC, 0x38, 0x0F, 0xB6, 0x44, 0x24,
    0x60, 0x4C, 0x8B, 0x15, 0x90, 0x2B, 0x4F, 0x00};
constexpr std::uint8_t kEventInstallerPrefix[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint8_t kDetour333390[]{
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x48,
    0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55};
constexpr std::uint8_t kDetour31de30[]{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44,
    0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53};
constexpr std::uint8_t kLuaGetInterfacePrefix[]{
    0x0F, 0xB6, 0xC2, 0x48, 0x8B, 0x44, 0xC1, 0x78,
    0xC3};
constexpr std::uint8_t kLuaRunStringOriginalPrefix[]{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50,
    0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01,
    0x00, 0x00, 0x48, 0x8B, 0x05};

constexpr std::array<CloneHookSpec, 8> kCloneHooks{{
    {0x7EA7D8, 17, 0x31AC30, 0x80A9B0, HookModule::Client,
     CloneInstallForm::Static, 0xB6A278, 0x681B30, 0x31B0D0,
     kDetour31ac30},
    {0x7EA7D8, 18, 0x31B1A0, 0x80A9B8, HookModule::Client,
     CloneInstallForm::Static, 0xB6A298, 0x681B60, 0x31B210,
     kDetour31b1a0},
    {0x7EA7D8, 19, 0x31B280, 0xB6BA40, HookModule::Client,
     CloneInstallForm::Static, 0xB6A2A8, 0x681B78, 0x31B300,
     kDetour31b280},
    {0x7EA800, 6, 0x300D80, 0x7EA938, HookModule::Client,
     CloneInstallForm::Static, 0xB69E60, 0x67FBF0, 0x3010C0,
     kDetour300d80},
    {0x7EA810, 21, 0x2F3EC0, 0x7EA908, HookModule::Client,
     CloneInstallForm::Static, 0xB69E40, 0x67F970, 0x2F4340,
     kDetour2f3ec0},
    {0x7EA810, 26, 0x31BD50, 0x80A9D0, HookModule::Client,
     CloneInstallForm::Static, 0xB6A2E8, 0x681BD8, 0x31C430,
     kDetour31bd50},
    {0x7EA878, 21, 0x2FDE50, 0x7EA918, HookModule::Client,
     CloneInstallForm::Static, 0xB69E50, 0x67FB98, 0x2FF160,
     kDetour2fde50},
    {0x7EA878, 23, 0x349AA0, 0x83C640, HookModule::Client,
     CloneInstallForm::Static, 0xB6B240, 0x682E78, 0x349AD0,
     kDetour349aa0},
}};

constexpr std::size_t kSkippedCreateMoveCloneIndex = kCloneHooks.size();

constexpr CloneHookSpec kEventHook{
    0x7EA840, 8, 0x333390, 0x80AA80, HookModule::Engine,
    CloneInstallForm::Generic, 0, 0, 0, kDetour333390};

constexpr std::uint32_t kEventRecordsBeginRva = 0xB6BA98;
constexpr std::uint32_t kEventRecordsEndRva = 0xB6BAA0;
constexpr std::uint32_t kEventRecordsCapacityRva = 0xB6BAA8;

struct MinHookSpec {
    std::uint32_t saved_slot_rva;
    HookModule module;
    std::uint32_t baseline_rva;
    std::uint32_t detour_rva;
};

constexpr std::array<MinHookSpec, 69> kMinHooks{{
    {0x83C630, HookModule::Client, 0xB49E0, 0x32C750},
    {0x80AA40, HookModule::Client, 0xBD1E0, 0x32C090},
    {0x823870, HookModule::Client, 0xBD210, 0x325260},
    {0x823848, HookModule::Client, 0xC6FA0, 0x3254A0},
    {0x7EA780, HookModule::Client, 0xCA270, 0x187400},
    {0x7EA798, HookModule::Client, 0xE0F30, 0x185EF0},
    {0x7EA670, HookModule::Client, 0x143120, 0x1869C0},
    {0x7EA790, HookModule::Client, 0xCAA40, 0x1850F0},
    {0x7EA720, HookModule::Client, 0x3A5D40, 0x1847B0},
    {0x7EA680, HookModule::Client, 0xDD300, 0x182850},
    {0x7EA7A0, HookModule::Client, 0xD9330, 0x182720},
    {0x7EA678, HookModule::Client, 0xE5B80, 0x1825C0},
    {0x7EA788, HookModule::Client, 0xCDEA0, 0x181A40},
    {0x7EA7C8, HookModule::Client, 0xCE010, 0x180FA0},
    {0x7EA6D8, HookModule::Client, 0xDE210, 0x17D530},
    {0x7EA718, HookModule::Client, 0xDE4E0, 0x17CFB0},
    {0x7EA668, HookModule::Client, 0xDE0E0, 0x17CB90},
    {0x7EA6B8, HookModule::Client, 0xDE730, 0x17C770},
    {0x7EA6E8, HookModule::Client, 0xDE380, 0x17C1C0},
    {0x7EA6F8, HookModule::Client, 0xDE400, 0x17BC10},
    {0x7EA6C8, HookModule::Client, 0xDE580, 0x17B4A0},
    {0x7EA6E0, HookModule::Client, 0xDE4B0, 0x17AE30},
    {0x7EA690, HookModule::Client, 0xDE700, 0x17A7B0},
    {0x7EA738, HookModule::Client, 0xDE150, 0x17A1E0},
    {0x7EA700, HookModule::Client, 0xDE480, 0x1793E0},
    {0x7EA6D0, HookModule::Client, 0xDEA90, 0x1773F0},
    {0x7EA698, HookModule::Client, 0xDED20, 0x1769A0},
    {0x7EA6A8, HookModule::Client, 0xDE960, 0x1760F0},
    {0x7EA730, HookModule::Client, 0xDF080, 0x175820},
    {0x7EA728, HookModule::Client, 0xDEBE0, 0x175200},
    {0x7EA6F0, HookModule::Client, 0xDEC40, 0x174BF0},
    {0x7EA6A0, HookModule::Client, 0xDEDF0, 0x174360},
    {0x7EA708, HookModule::Client, 0xDECA0, 0x173DB0},
    {0x7EA6B0, HookModule::Client, 0xDEF50, 0x1737D0},
    {0x7EA6C0, HookModule::Client, 0xDE9A0, 0x172CA0},
    {0x7EA688, HookModule::Client, 0xDDF40, 0x177E10},
    {0x7EA710, HookModule::Client, 0xDE7A0, 0x1783C0},
    {0x7EA740, HookModule::Client, 0xDE820, 0x172430},
    {0x7EA770, HookModule::Client, 0xD73E0, 0x1717F0},
    {0x7EA750, HookModule::Client, 0xD74D0, 0x170B80},
    {0x7EA7B8, HookModule::Client, 0xD74A0, 0x16FF10},
    {0x7EA7C0, HookModule::Client, 0xD7470, 0x16F390},
    {0x7EA7B0, HookModule::Client, 0xD7410, 0x16E7B0},
    {0x7EA768, HookModule::Client, 0xD7380, 0x16DE10},
    {0x7EA7A8, HookModule::Client, 0xD7350, 0x16D480},
    {0x7EA760, HookModule::Client, 0xD7320, 0x16C830},
    {0x7EA758, HookModule::Client, 0xD7440, 0x16B990},
    {0x7EA748, HookModule::Client, 0xD73B0, 0x16AC50},
    {0x80A9C0, HookModule::Client, 0x73390, 0x31BAF0},
    {0x7EA778, HookModule::Engine, 0x1F67F0, 0x187B70},
    {0xB6CA28, HookModule::Engine, 0x96970, 0x322530},
    {0x80AA48, HookModule::Engine, 0x1E7960, 0x333290},
    {0x7EA920, HookModule::Engine, 0x1EABD0, 0x2FB950},
    {0x80AA58, HookModule::Engine, 0x1EA320, 0x332DC0},
    {0x823858, HookModule::Client, 0x1B5230, 0x32DF20},
    {0x80A9F0, HookModule::LuaShared, 0x1A9C0, 0x323F90},
    {0x80AA30, HookModule::LuaShared, 0x1C6C0, 0x325060},
    {0x80AA68, HookModule::LuaShared, 0x19070, 0x325160},
    {0x80AA20, HookModule::LuaShared, 0x21470, 0x32D400},
    {0x80A9E8, HookModule::Client, 0x196EB0, 0x335860},
    {0x80AA00, HookModule::Client, 0x206540, 0x335830},
    {0x80AA08, HookModule::Client, 0x2F1250, 0x3358F0},
    {0x80AA18, HookModule::Engine, 0x223D50, 0x335950},
    {0x80AA38, HookModule::Client, 0x26C0F0, 0x3359B0},
    {0x80AA10, HookModule::Client, 0x2058C0, 0x3363F0},
    {0x80AA50, HookModule::Client, 0x1BA0F0, 0x336340},
    {0x83C628, HookModule::Client, 0x9E7F0, 0x335A00},
    {0x7EA578, HookModule::LuaShared, 0x1E000, 0x58230},
    {0x7EA170, HookModule::LuaShared, 0x16880, 0x56E70},
}};

static_assert(kirkware_hook_fingerprints::kMinHookTargets.size() ==
              kMinHooks.size());
static_assert(kirkware_hook_normalized::kMinHookTargets.size() ==
              kMinHooks.size());
constexpr bool NormalizedFingerprintLayoutValid() {
    std::size_t eligible = 0;
    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        const auto& fingerprint =
            kirkware_hook_normalized::kMinHookTargets[index];
        if (fingerprint.baseline_rva != kMinHooks[index].baseline_rva)
            return false;
        if (fingerprint.domain ==
            kirkware_hook_normalized::CandidateDomain::CurrentExactOnly) {
            if (fingerprint.blob_offset != 0xffffffffu || fingerprint.span ||
                fingerprint.fixed_bytes || fingerprint.anchor_offset ||
                fingerprint.anchor_length)
                return false;
            continue;
        }
        ++eligible;
        if (!fingerprint.span || !fingerprint.fixed_bytes ||
            fingerprint.fixed_bytes > fingerprint.span ||
            fingerprint.blob_offset >
                kirkware_hook_normalized::kPatternBytes.size() ||
            fingerprint.span >
                kirkware_hook_normalized::kPatternBytes.size() -
                    fingerprint.blob_offset ||
            !fingerprint.anchor_length ||
            fingerprint.anchor_offset > fingerprint.span ||
            fingerprint.anchor_length >
                fingerprint.span - fingerprint.anchor_offset)
            return false;
        std::size_t fixed = 0;
        for (std::size_t offset = 0; offset != fingerprint.span; ++offset) {
            const std::uint8_t mask =
                kirkware_hook_normalized::kPatternMasks[
                    fingerprint.blob_offset + offset];
            if (mask != 0 && mask != 0xff)
                return false;
            fixed += mask != 0;
        }
        if (fixed != fingerprint.fixed_bytes)
            return false;
        for (std::size_t offset = 0; offset != fingerprint.anchor_length;
             ++offset) {
            if (kirkware_hook_normalized::kPatternMasks[
                    fingerprint.blob_offset + fingerprint.anchor_offset +
                    offset] != 0xff)
                return false;
        }
    }
    return eligible == kirkware_hook_normalized::kEligibleCount;
}
static_assert(NormalizedFingerprintLayoutValid());
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kBreakLcHookIndex].prefix ==
    std::array<std::uint8_t, 5>{{0x48, 0x83, 0xEC, 0x78, 0x48}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kLuaVmGuardHookIndex]
            .prefix ==
    std::array<std::uint8_t, 5>{{0x40, 0x56, 0x57, 0x41, 0x55}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets
            [kBoneBuildTransformationsHookIndex]
                .prefix ==
    std::array<std::uint8_t, 5>{{0x4C, 0x8B, 0xDC, 0x48, 0x81}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets
            [kTransitionShutdownHookIndex]
                .prefix ==
    std::array<std::uint8_t, 5>{{0x48, 0x89, 0x5C, 0x24, 0x08}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kMotionBlurHookIndex]
            .prefix ==
    std::array<std::uint8_t, 5>{{0x40, 0x55, 0x53, 0x56, 0x41}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kHostFilterTimeHookIndex]
            .prefix ==
    std::array<std::uint8_t, 5>{{0x40, 0x53, 0x48, 0x83, 0xEC}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kCamThinkHookIndex]
            .prefix ==
    std::array<std::uint8_t, 5>{{0x40, 0x55, 0x53, 0x48, 0x8D}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets
            [kClientLuaTeardownHookIndex]
                .prefix ==
    std::array<std::uint8_t, 5>{{0x40, 0x57, 0x48, 0x83, 0xEC}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets
            [kLocalShouldInterpolateHookIndex]
                .prefix ==
    std::array<std::uint8_t, 5>{{0x48, 0x89, 0x5C, 0x24, 0x08}});
static_assert(
    kirkware_hook_fingerprints::kMinHookTargets[kClientEventHookIndex]
            .prefix ==
    std::array<std::uint8_t, 5>{{0x48, 0x89, 0x5C, 0x24, 0x10}});
static_assert(kMinHooks[kClMoveHookIndex].module == HookModule::Engine &&
              kMinHooks[kClMoveHookIndex].baseline_rva == 0x96970 &&
              kMinHooks[kClMoveHookIndex].detour_rva == 0x322530);
static_assert(
    kMinHooks[kNetChannelProcessPacketHookIndex].module ==
            HookModule::Engine &&
        kMinHooks[kNetChannelProcessPacketHookIndex].saved_slot_rva ==
            0x80AA48 &&
        kMinHooks[kNetChannelProcessPacketHookIndex].detour_rva == 0x333290);
static_assert(
    kMinHooks[kNetChannelSendNetMsgHookIndex].module == HookModule::Engine &&
        kMinHooks[kNetChannelSendNetMsgHookIndex].saved_slot_rva == 0x7EA920 &&
        kMinHooks[kNetChannelSendNetMsgHookIndex].detour_rva == 0x2FB950);
static_assert(
    kMinHooks[kNetChannelSendDatagramHookIndex].module ==
            HookModule::Engine &&
        kMinHooks[kNetChannelSendDatagramHookIndex].saved_slot_rva ==
            0x80AA58 &&
        kMinHooks[kNetChannelSendDatagramHookIndex].detour_rva == 0x332DC0);
static_assert(kMinHooks[kBreakLcHookIndex].module == HookModule::Client &&
              kMinHooks[kBreakLcHookIndex].saved_slot_rva == 0x823858 &&
              kMinHooks[kBreakLcHookIndex].detour_rva == 0x32DF20);
static_assert(
    kMinHooks[kLuaRunStringHookIndex].module == HookModule::LuaShared &&
        kMinHooks[kLuaRunStringHookIndex].saved_slot_rva == 0x80A9F0 &&
        kMinHooks[kLuaRunStringHookIndex].detour_rva == 0x323F90);
static_assert(
    kMinHooks[kLuaFilterAdvanceHookIndex].module == HookModule::LuaShared &&
        kMinHooks[kLuaFilterAdvanceHookIndex].saved_slot_rva == 0x80AA30 &&
        kMinHooks[kLuaFilterAdvanceHookIndex].detour_rva == 0x325060);
static_assert(
    kMinHooks[kLuaFilterAdvanceIntegerHookIndex].module ==
            HookModule::LuaShared &&
        kMinHooks[kLuaFilterAdvanceIntegerHookIndex].saved_slot_rva ==
            0x80AA68 &&
        kMinHooks[kLuaFilterAdvanceIntegerHookIndex].detour_rva == 0x325160);
static_assert(
    kMinHooks[kLuaVmGuardHookIndex].module == HookModule::LuaShared &&
        kMinHooks[kLuaVmGuardHookIndex].saved_slot_rva == 0x80AA20 &&
        kMinHooks[kLuaVmGuardHookIndex].detour_rva == 0x32D400);
static_assert(
    kMinHooks[kBoneBuildTransformationsHookIndex].module ==
            HookModule::Client &&
        kMinHooks[kBoneBuildTransformationsHookIndex].saved_slot_rva ==
            0x80A9E8 &&
        kMinHooks[kBoneBuildTransformationsHookIndex].detour_rva == 0x335860);
static_assert(
    kMinHooks[kTransitionShutdownHookIndex].module == HookModule::Client &&
        kMinHooks[kTransitionShutdownHookIndex].saved_slot_rva == 0x80AA00 &&
        kMinHooks[kTransitionShutdownHookIndex].detour_rva == 0x335830);
static_assert(kMinHooks[kMotionBlurHookIndex].module == HookModule::Client &&
              kMinHooks[kMotionBlurHookIndex].saved_slot_rva == 0x80AA08 &&
              kMinHooks[kMotionBlurHookIndex].detour_rva == 0x3358F0);
static_assert(kMinHooks[kHostFilterTimeHookIndex].module == HookModule::Engine &&
              kMinHooks[kHostFilterTimeHookIndex].saved_slot_rva == 0x80AA18 &&
              kMinHooks[kHostFilterTimeHookIndex].detour_rva == 0x335950);
static_assert(kMinHooks[kCamThinkHookIndex].module == HookModule::Client &&
              kMinHooks[kCamThinkHookIndex].saved_slot_rva == 0x80AA38 &&
              kMinHooks[kCamThinkHookIndex].detour_rva == 0x3359B0);
static_assert(
    kMinHooks[kClientLuaTeardownHookIndex].module == HookModule::Client &&
        kMinHooks[kClientLuaTeardownHookIndex].saved_slot_rva == 0x80AA10 &&
        kMinHooks[kClientLuaTeardownHookIndex].detour_rva == 0x3363F0);
static_assert(
    kMinHooks[kLocalShouldInterpolateHookIndex].module == HookModule::Client &&
        kMinHooks[kLocalShouldInterpolateHookIndex].saved_slot_rva ==
            0x80AA50 &&
        kMinHooks[kLocalShouldInterpolateHookIndex].detour_rva == 0x336340);
static_assert(kMinHooks[kClientEventHookIndex].module == HookModule::Client &&
              kMinHooks[kClientEventHookIndex].saved_slot_rva == 0x83C628 &&
              kMinHooks[kClientEventHookIndex].detour_rva == 0x335A00);

struct ExecutableSection {
    std::uint32_t rva = 0;
    std::vector<std::uint8_t> bytes;
};

bool LoadExecutableSections(HANDLE process, const ModuleInfo& module,
                            std::vector<ExecutableSection>& output,
                            Failure& failure, std::size_t module_index) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadValue(process, module.base, dos))
        return Fail(failure, "fingerprint_image", "dos_read", module_index,
                    module.base, 0, 0, GetLastError());
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::uint64_t>(dos.e_lfanew) > module.size ||
        sizeof(IMAGE_NT_HEADERS64) >
            module.size - static_cast<std::uint64_t>(dos.e_lfanew))
        return Fail(failure, "fingerprint_image", "dos_exact", module_index,
                    module.base, dos.e_magic, IMAGE_DOS_SIGNATURE);
    const std::uint64_t nt_address =
        module.base + static_cast<std::uint32_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(process, nt_address, nt))
        return Fail(failure, "fingerprint_image", "nt_read", module_index,
                    nt_address, 0, 0, GetLastError());
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage != module.size)
        return Fail(failure, "fingerprint_image", "nt_exact", module_index,
                    nt_address, nt.OptionalHeader.SizeOfImage, module.size);
    const std::uint64_t section_address =
        nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (section_address < nt_address ||
        section_address - module.base > module.size ||
        section_bytes > module.size - (section_address - module.base))
        return Fail(failure, "fingerprint_image", "sections_range",
                    module_index, section_address, section_bytes,
                    module.size);
    std::vector<IMAGE_SECTION_HEADER> sections(
        nt.FileHeader.NumberOfSections);
    if (!ReadExact(process, section_address, sections.data(), section_bytes))
        return Fail(failure, "fingerprint_image", "sections_read",
                    module_index, section_address, 0, section_bytes,
                    GetLastError());
    for (const auto& section : sections) {
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        std::uint64_t extent = section.Misc.VirtualSize;
        if (!extent)
            extent = section.SizeOfRawData;
        const std::uint64_t rva = section.VirtualAddress;
        if (!extent || rva >= module.size || extent > module.size - rva)
            return Fail(failure, "fingerprint_image", "section_range",
                        module_index, module.base + rva, extent,
                        module.size - rva);
        ExecutableSection loaded{};
        loaded.rva = static_cast<std::uint32_t>(rva);
        loaded.bytes.resize(static_cast<std::size_t>(extent));
        if (!ReadExact(process, module.base + rva, loaded.bytes.data(),
                       loaded.bytes.size()))
            return Fail(failure, "fingerprint_image", "section_read",
                        module_index, module.base + rva, 0, extent,
                        GetLastError());
        output.push_back(std::move(loaded));
    }
    if (output.empty())
        return Fail(failure, "fingerprint_image", "executable_present",
                    module_index, module.base, 0, 1);
    return true;
}

bool FingerprintBodyMatches(
    const ExecutableSection& section, std::size_t offset,
    const kirkware_hook_fingerprints::Fingerprint& fingerprint) {
    for (std::size_t index = 0; index != fingerprint.offsets.size();
         ++index) {
        const std::size_t byte_offset = fingerprint.offsets[index];
        if (byte_offset >= section.bytes.size() - offset ||
            section.bytes[offset + byte_offset] != fingerprint.values[index])
            return false;
    }
    return true;
}

bool FingerprintPrefixMatches(
    const ExecutableSection& section, std::size_t offset,
    const kirkware_hook_fingerprints::Fingerprint& fingerprint) {
    if (fingerprint.prefix.size() > section.bytes.size() - offset)
        return false;
    for (std::size_t index = 0; index != fingerprint.prefix.size(); ++index) {
        if ((section.bytes[offset + index] & fingerprint.prefix_mask[index]) !=
            (fingerprint.prefix[index] & fingerprint.prefix_mask[index]))
            return false;
    }
    return true;
}

enum class FingerprintMatchState : std::uint8_t {
    NotFound,
    Unique,
    Ambiguous,
};

struct FingerprintSearchResult {
    FingerprintMatchState state = FingerprintMatchState::NotFound;
    std::uint32_t rva = 0;
    std::size_t count = 0;
};

void RecordFingerprintMatch(FingerprintSearchResult& result,
                            std::uint32_t rva) {
    if (result.count == 0)
        result.rva = rva;
    ++result.count;
}

void FinishFingerprintSearch(FingerprintSearchResult& result) {
    result.state = result.count == 0
        ? FingerprintMatchState::NotFound
        : result.count == 1
            ? FingerprintMatchState::Unique
            : FingerprintMatchState::Ambiguous;
}

FingerprintSearchResult FindExactMinHookMatches(
    const std::vector<ExecutableSection>& sections,
    const kirkware_hook_fingerprints::Fingerprint& fingerprint,
    std::uint64_t saved) {
    FingerprintSearchResult result{};
    const auto scan = [&](bool aligned) {
        for (const auto& section : sections) {
            if (section.bytes.size() <= fingerprint.offsets.back())
                continue;
            for (std::size_t offset = 0;
                 offset + fingerprint.offsets.back() < section.bytes.size();
                 ++offset) {
                const std::uint32_t rva =
                    section.rva + static_cast<std::uint32_t>(offset);
                if (((rva & 15u) == 0) != aligned)
                    continue;
                const bool prefix_candidate =
                    (section.bytes[offset] & fingerprint.prefix_mask[0]) ==
                    (fingerprint.prefix[0] & fingerprint.prefix_mask[0]);
                const bool installed_candidate =
                    section.bytes[offset] == 0xE9 && saved != 0;
                if ((!prefix_candidate && !installed_candidate) ||
                    !FingerprintBodyMatches(section, offset, fingerprint))
                    continue;
                if (!FingerprintPrefixMatches(
                        section, offset, fingerprint) &&
                    !installed_candidate)
                    continue;
                RecordFingerprintMatch(result, rva);
            }
        }
    };
    scan(true);
    if (result.count == 0)
        scan(false);
    FinishFingerprintSearch(result);
    return result;
}

bool NormalizedFingerprintMatches(
    const ExecutableSection& section, std::size_t offset,
    const kirkware_hook_normalized::Fingerprint& fingerprint) {
    if (offset > section.bytes.size() ||
        fingerprint.span > section.bytes.size() - offset)
        return false;
    const std::size_t blob = fingerprint.blob_offset;
    const std::size_t anchor = offset + fingerprint.anchor_offset;
    if (std::memcmp(
            section.bytes.data() + anchor,
            kirkware_hook_normalized::kPatternBytes.data() + blob +
                fingerprint.anchor_offset,
            fingerprint.anchor_length) != 0)
        return false;
    for (std::size_t index = 0; index != fingerprint.span; ++index) {
        const std::uint8_t mask =
            kirkware_hook_normalized::kPatternMasks[blob + index];
        if ((section.bytes[offset + index] & mask) !=
            (kirkware_hook_normalized::kPatternBytes[blob + index] &
             mask))
            return false;
    }
    return true;
}

bool LoadRuntimeFunctionStarts(HANDLE process, const ModuleInfo& module,
                               std::vector<std::uint32_t>& output,
                               Failure& failure, std::size_t index) {
    output.clear();
    IMAGE_DOS_HEADER dos{};
    if (!ReadValue(process, module.base, dos))
        return Fail(failure, "fingerprint_normalized", "dos_read", index,
                    module.base, 0, 0, GetLastError());
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::uint64_t>(dos.e_lfanew) > module.size ||
        sizeof(IMAGE_NT_HEADERS64) >
            module.size - static_cast<std::uint64_t>(dos.e_lfanew))
        return Fail(failure, "fingerprint_normalized", "dos_exact", index,
                    module.base, dos.e_magic, IMAGE_DOS_SIGNATURE);
    const std::uint64_t nt_address =
        module.base + static_cast<std::uint32_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(process, nt_address, nt))
        return Fail(failure, "fingerprint_normalized", "nt_read", index,
                    nt_address, 0, 0, GetLastError());
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != module.size ||
        nt.OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return Fail(failure, "fingerprint_normalized", "nt_exact", index,
                    nt_address, nt.OptionalHeader.SizeOfImage, module.size);
    const IMAGE_DATA_DIRECTORY directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress ||
        directory.Size < sizeof(RUNTIME_FUNCTION) ||
        directory.Size % sizeof(RUNTIME_FUNCTION) != 0 ||
        directory.VirtualAddress >= module.size ||
        directory.Size > module.size - directory.VirtualAddress ||
        directory.Size / sizeof(RUNTIME_FUNCTION) > (1u << 20u))
        return Fail(failure, "fingerprint_normalized", "exception_range",
                    index, module.base + directory.VirtualAddress,
                    directory.Size, module.size);
    std::vector<RUNTIME_FUNCTION> functions(
        directory.Size / sizeof(RUNTIME_FUNCTION));
    if (!ReadExact(process, module.base + directory.VirtualAddress,
                   functions.data(), directory.Size))
        return Fail(failure, "fingerprint_normalized", "exception_read",
                    index, module.base + directory.VirtualAddress, 0,
                    directory.Size, GetLastError());
    output.reserve(functions.size());
    for (const auto& function : functions) {
        if (function.BeginAddress >= function.EndAddress ||
            function.EndAddress > module.size)
            return Fail(failure, "fingerprint_normalized",
                        "exception_entry", index,
                        module.base + function.BeginAddress,
                        function.EndAddress, module.size);
        output.push_back(function.BeginAddress);
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    if (output.empty())
        return Fail(failure, "fingerprint_normalized", "exception_present",
                    index, module.base + directory.VirtualAddress, 0, 1);
    return true;
}

FingerprintSearchResult FindNormalizedMinHookMatches(
    const std::vector<ExecutableSection>& sections,
    const kirkware_hook_normalized::Fingerprint& fingerprint,
    const std::vector<std::uint32_t>& runtime_function_starts) {
    FingerprintSearchResult result{};
    if (fingerprint.domain ==
        kirkware_hook_normalized::CandidateDomain::ExecutableBytes) {
        for (const auto& section : sections) {
            if (section.bytes.size() < fingerprint.span)
                continue;
            for (std::size_t offset = 0;
                 offset <= section.bytes.size() - fingerprint.span;
                 ++offset) {
                if (!NormalizedFingerprintMatches(
                        section, offset, fingerprint))
                    continue;
                RecordFingerprintMatch(
                    result,
                    section.rva + static_cast<std::uint32_t>(offset));
            }
        }
    } else if (
        fingerprint.domain ==
        kirkware_hook_normalized::CandidateDomain::
            RuntimeFunctionStarts) {
        for (std::uint32_t rva : runtime_function_starts) {
            for (const auto& section : sections) {
                if (rva < section.rva)
                    continue;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(rva) - section.rva;
                if (offset > section.bytes.size() ||
                    fingerprint.span > section.bytes.size() - offset ||
                    !NormalizedFingerprintMatches(
                        section, static_cast<std::size_t>(offset),
                        fingerprint))
                    continue;
                RecordFingerprintMatch(result, rva);
                break;
            }
        }
    }
    FinishFingerprintSearch(result);
    return result;
}

bool ResolveMinHookTarget(
    HANDLE process, std::uint64_t image_base, const MinHookSpec& spec,
    const ModuleInfo& module,
    const std::vector<ExecutableSection>& sections,
    const kirkware_hook_fingerprints::Fingerprint& fingerprint,
    const kirkware_hook_normalized::Fingerprint& normalized,
    std::uint32_t& resolved_rva, Failure& failure, std::size_t index) {
    std::uint64_t saved = 0;
    const std::uint64_t saved_slot = image_base + spec.saved_slot_rva;
    if (!ReadValue(process, saved_slot, saved))
        return Fail(failure, "fingerprint", "saved_read", index,
                    saved_slot, 0, 0, GetLastError());
    const FingerprintSearchResult exact =
        FindExactMinHookMatches(sections, fingerprint, saved);
    if (exact.state == FingerprintMatchState::Ambiguous)
        return Fail(failure, "fingerprint", "exact_unique", index,
                    module.base + exact.rva, exact.count, 1);
    if (exact.state == FingerprintMatchState::Unique) {
        resolved_rva = exact.rva;
    } else {
        if (saved != 0)
            return Fail(failure, "fingerprint", "installed_exact_present",
                        index, module.base, 0, 1);
        if (normalized.domain ==
            kirkware_hook_normalized::CandidateDomain::CurrentExactOnly)
            return Fail(failure, "fingerprint", "exact_present", index,
                        module.base, 0, 1);
        std::vector<std::uint32_t> runtime_function_starts;
        if (normalized.domain ==
                kirkware_hook_normalized::CandidateDomain::
                    RuntimeFunctionStarts &&
            !LoadRuntimeFunctionStarts(process, module,
                                       runtime_function_starts, failure,
                                       index))
            return false;
        const FingerprintSearchResult fallback =
            FindNormalizedMinHookMatches(
                sections, normalized, runtime_function_starts);
        if (fallback.state != FingerprintMatchState::Unique)
            return Fail(failure, "fingerprint_normalized", "unique", index,
                        fallback.count ? module.base + fallback.rva
                                       : module.base,
                        fallback.count, 1);
        resolved_rva = fallback.rva;
    }
    if (!RangeInModule(module.base + resolved_rva, kTargetPrefixSize,
                       module) ||
        !ExecutableRange(process, module.base + resolved_rva,
                         kTargetPrefixSize))
        return Fail(failure, "fingerprint", "resolved_executable", index,
                    module.base + resolved_rva, resolved_rva, module.size);
    return true;
}

bool VerifyNetChannelMinHookTopology(
    HANDLE process, const ModuleInfo& engine,
    const std::array<std::uint32_t, kMinHooks.size()>& resolved,
    Failure& failure) {
    constexpr std::size_t kProcessPacketSlot = 39;
    constexpr std::size_t kSendNetMsgSlot = 40;
    constexpr std::size_t kSendDatagramSlot = 46;
    constexpr std::size_t kSlotDistance =
        (kSendDatagramSlot - kProcessPacketSlot) * sizeof(std::uint64_t);
    constexpr std::size_t kVtablePrefix =
        kProcessPacketSlot * sizeof(std::uint64_t);

    IMAGE_DOS_HEADER dos{};
    if (!ReadValue(process, engine.base, dos))
        return Fail(failure, "netchannel_topology", "dos_read", 0,
                    engine.base, 0, 0, GetLastError());
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::uint64_t>(dos.e_lfanew) > engine.size ||
        sizeof(IMAGE_NT_HEADERS64) >
            engine.size - static_cast<std::uint64_t>(dos.e_lfanew))
        return Fail(failure, "netchannel_topology", "dos_exact", 0,
                    engine.base, dos.e_magic, IMAGE_DOS_SIGNATURE);

    const std::uint64_t nt_address =
        engine.base + static_cast<std::uint32_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(process, nt_address, nt))
        return Fail(failure, "netchannel_topology", "nt_read", 0,
                    nt_address, 0, 0, GetLastError());
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage != engine.size)
        return Fail(failure, "netchannel_topology", "nt_exact", 0,
                    nt_address, nt.OptionalHeader.SizeOfImage, engine.size);

    const std::uint64_t section_address =
        nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (section_address < nt_address ||
        section_address - engine.base > engine.size ||
        section_bytes > engine.size - (section_address - engine.base))
        return Fail(failure, "netchannel_topology", "sections_range", 0,
                    section_address, section_bytes, engine.size);
    std::vector<IMAGE_SECTION_HEADER> sections(
        nt.FileHeader.NumberOfSections);
    if (!ReadExact(process, section_address, sections.data(), section_bytes))
        return Fail(failure, "netchannel_topology", "sections_read", 0,
                    section_address, 0, section_bytes, GetLastError());

    const std::uint64_t process_packet =
        engine.base + resolved[kNetChannelProcessPacketHookIndex];
    const std::uint64_t send_net_msg =
        engine.base + resolved[kNetChannelSendNetMsgHookIndex];
    const std::uint64_t send_datagram =
        engine.base + resolved[kNetChannelSendDatagramHookIndex];
    std::size_t match_count = 0;
    std::uint64_t first_vtable = 0;
    bool found_rdata = false;
    for (const auto& section : sections) {
        if (std::memcmp(section.Name, ".rdata", 6) != 0 ||
            section.Name[6] != 0)
            continue;
        found_rdata = true;
        std::uint64_t extent = section.Misc.VirtualSize;
        if (!extent)
            extent = section.SizeOfRawData;
        const std::uint64_t rva = section.VirtualAddress;
        if (!extent || rva >= engine.size || extent > engine.size - rva)
            return Fail(failure, "netchannel_topology", "rdata_range", 0,
                        engine.base + rva, extent, engine.size - rva);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent));
        if (!ReadExact(process, engine.base + rva, bytes.data(), bytes.size()))
            return Fail(failure, "netchannel_topology", "rdata_read", 0,
                        engine.base + rva, 0, bytes.size(), GetLastError());
        for (std::size_t offset = 0;
             offset + kSlotDistance + sizeof(std::uint64_t) <= bytes.size();
             offset += sizeof(std::uint64_t)) {
            std::uint64_t observed_process_packet = 0;
            std::uint64_t observed_send_net_msg = 0;
            std::uint64_t observed_send_datagram = 0;
            std::memcpy(&observed_process_packet, bytes.data() + offset,
                        sizeof(observed_process_packet));
            std::memcpy(&observed_send_net_msg,
                        bytes.data() + offset +
                            (kSendNetMsgSlot - kProcessPacketSlot) *
                                sizeof(std::uint64_t),
                        sizeof(observed_send_net_msg));
            std::memcpy(&observed_send_datagram,
                        bytes.data() + offset + kSlotDistance,
                        sizeof(observed_send_datagram));
            if (observed_process_packet != process_packet ||
                observed_send_net_msg != send_net_msg ||
                observed_send_datagram != send_datagram ||
                offset < kVtablePrefix)
                continue;
            const std::uint64_t candidate =
                engine.base + rva + offset - kVtablePrefix;
            if (!RangeInModule(candidate,
                               (kSendDatagramSlot + 1) *
                                   sizeof(std::uint64_t),
                               engine))
                continue;
            if (!match_count)
                first_vtable = candidate;
            ++match_count;
        }
    }
    if (!found_rdata)
        return Fail(failure, "netchannel_topology", "rdata_present", 0,
                    engine.base, 0, 1);
    if (match_count != 1)
        return Fail(failure, "netchannel_topology", "vtable_unique", 0,
                    first_vtable ? first_vtable : engine.base, match_count,
                    1);
    return true;
}

bool ResolveMinHookTargets(
    HANDLE process, std::uint64_t image_base, const ModuleInfo& client,
    const ModuleInfo& engine, const ModuleInfo& lua_shared,
    std::array<std::uint32_t, kMinHooks.size()>& resolved,
    Failure& failure) {
    std::vector<ExecutableSection> client_sections;
    std::vector<ExecutableSection> engine_sections;
    std::vector<ExecutableSection> lua_shared_sections;
    if (!LoadExecutableSections(process, client, client_sections, failure,
                                0) ||
        !LoadExecutableSections(process, engine, engine_sections, failure,
                                1) ||
        !LoadExecutableSections(process, lua_shared, lua_shared_sections,
                                failure, 2))
        return false;
    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        const auto& spec = kMinHooks[index];
        const ModuleInfo* module = nullptr;
        const std::vector<ExecutableSection>* sections = nullptr;
        if (spec.module == HookModule::Client) {
            module = &client;
            sections = &client_sections;
        } else if (spec.module == HookModule::Engine) {
            module = &engine;
            sections = &engine_sections;
        } else if (spec.module == HookModule::LuaShared) {
            module = &lua_shared;
            sections = &lua_shared_sections;
        } else {
            return Fail(failure, "fingerprint", "module_supported", index,
                        spec.baseline_rva, static_cast<std::uint8_t>(spec.module),
                        static_cast<std::uint8_t>(HookModule::LuaShared));
        }
        if (!ResolveMinHookTarget(
                process, image_base, spec, *module, *sections,
                kirkware_hook_fingerprints::kMinHookTargets[index],
                kirkware_hook_normalized::kMinHookTargets[index],
                resolved[index], failure, index))
            return false;
        for (std::size_t previous = 0; previous != index; ++previous) {
            if (kMinHooks[previous].module == spec.module &&
                resolved[previous] == resolved[index])
                return Fail(failure, "fingerprint", "targets_distinct",
                            index, module->base + resolved[index], previous,
                            index);
        }
    }
    return VerifyNetChannelMinHookTopology(process, engine, resolved,
                                           failure);
}

const ModuleInfo& ModuleFor(HookModule module, const ModuleInfo& client,
                            const ModuleInfo& engine,
                            const ModuleInfo& lua_shared,
                            const ModuleInfo& vgui2) {
    switch (module) {
    case HookModule::Client:
        return client;
    case HookModule::Engine:
        return engine;
    case HookModule::LuaShared:
        return lua_shared;
    case HookModule::Vgui2:
        return vgui2;
    }
    return client;
}

bool VerifyBytes(HANDLE process, std::uint64_t address,
                 const std::uint8_t* expected, std::size_t size,
                 Failure& failure, const char* stage,
                 const char* predicate, std::size_t index) {
    std::array<std::uint8_t, 64> observed{};
    if (!expected || !size || size > observed.size())
        return Fail(failure, stage, "expected_prefix_size", index, address,
                    size, observed.size());
    if (!ReadExact(process, address, observed.data(), size))
        return Fail(failure, stage, "prefix_read", index, address, 0, 0,
                    GetLastError());
    if (std::memcmp(observed.data(), expected, size) != 0)
        return Fail(failure, stage, predicate, index, address,
                    PrefixValue(observed.data(), size),
                    PrefixValue(expected, size));
    return true;
}

bool VerifyOriginal(HANDLE process, const DirectHookSpec& spec,
                    const ModuleInfo& module, std::uint64_t original,
                    Failure& failure, std::size_t index,
                    const char* stage) {
    if (!RangeInModule(original, 1, module) ||
        !ExecutableRange(process, original))
        return Fail(failure, stage, "original_executable", index,
                    original, 0, module.base);
    if (spec.module == HookModule::Engine && spec.vtable_index == 95) {
        std::array<std::uint8_t, 12> thunk{};
        if (!ReadExact(process, original, thunk.data(), thunk.size()))
            return Fail(failure, stage, "thunk_read", index,
                        original, 0, 0, GetLastError());
        std::int32_t displacement = 0;
        std::memcpy(&displacement, thunk.data() + 1, sizeof(displacement));
        const std::uint64_t destination = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(original + 5) + displacement);
        if (thunk[0] != 0xE9 ||
            !RangeInModule(destination, 1, module) ||
            !ExecutableRange(process, destination) ||
            !std::all_of(thunk.begin() + 5, thunk.end(),
                         [](std::uint8_t byte) { return byte == 0xCC; }))
            return Fail(failure, stage, "thunk_structure", index,
                        original, destination, module.base);
        return true;
    }
    if (!VerifyBytes(process, original, spec.original_prefix,
                     spec.original_prefix_size, failure, stage,
                     "original_prefix", index))
        return false;
    if (spec.module == HookModule::Client && spec.vtable_index == 35) {
        std::array<std::uint8_t, 13> prefix{};
        if (!ReadExact(process, original, prefix.data(), prefix.size()))
            return Fail(failure, stage, "client_prefix_read", index,
                        original, 0, 0, GetLastError());
        if (prefix[10] != 0x83 || prefix[11] != 0xFA || prefix[12] != 0x06)
            return Fail(failure, stage, "client_type_check", index,
                        original + 10,
                        static_cast<std::uint64_t>(prefix[10]) |
                            (static_cast<std::uint64_t>(prefix[11]) << 8) |
                            (static_cast<std::uint64_t>(prefix[12]) << 16),
                        0x06FA83);
    }
    return true;
}

struct DirectState {
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry_slot = 0;
    std::uint64_t original = 0;
    bool installed = false;
    bool saved_present = false;
    bool saved_changed = false;
    bool entry_changed = false;
    bool changed = false;
};

constexpr std::array<std::uint8_t, 28> kModelRenderCallFingerprint{{
    0x48, 0x8B, 0xCF, 0xF3, 0x0F, 0x11, 0x44, 0x24,
    0x28, 0x44, 0x89, 0x6C, 0x24, 0x20, 0xE8, 0x63,
    0x01, 0x00, 0x00, 0x8B, 0x8C, 0x24, 0xC0, 0x00,
    0x00, 0x00, 0x03, 0xC8,
}};
constexpr std::size_t kModelRenderCallOffset = 14;
constexpr std::array<std::uint8_t, 5> kModelRenderOriginalCall{{
    0xE8, 0x63, 0x01, 0x00, 0x00,
}};
constexpr std::array<std::uint8_t, 21> kModelRenderCalleePrefix{{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC,
    0x24, 0xA8, 0xFD, 0xFF, 0xFF,
}};
constexpr std::array<std::uint8_t, 15> kModelRenderFaultShape{{
    0x40, 0x38, 0x7B, 0x38, 0x74, 0x3B, 0x48, 0x8B,
    0x0B, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10,
}};
constexpr std::array<std::uint8_t, 15> kModelRenderLegacyFaultShape{{
    0x44, 0x38, 0x77, 0x38, 0x74, 0x3B, 0x48, 0x8B,
    0x0F, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10,
}};
constexpr std::array<std::uint8_t, 56> kModelRenderGuardCode{{
    0x49, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x25,
    0xA8, 0x07, 0x75, 0x21, 0x48, 0x3D, 0x00, 0x00,
    0x01, 0x00, 0x72, 0x19, 0x49, 0xBA, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x4C, 0x39,
    0xD0, 0x73, 0x0A, 0x4C, 0x8B, 0x15, 0x06, 0x00,
    0x00, 0x00, 0x41, 0xFF, 0xE2, 0x31, 0xC0, 0xC3,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}};

struct ModelRenderGuardState {
    std::uint64_t target = 0;
    std::uint64_t original_target = 0;
    std::uint64_t allocation = 0;
    std::array<std::uint8_t, kModelRenderGuardCode.size()> code{};
    std::array<std::uint8_t, kModelRenderOriginalCall.size()> replacement{};
    DWORD protection = 0;
    bool protection_changed = false;
    bool target_may_be_changed = false;
    bool installed = false;
    bool allocation_owned = false;
    bool changed = false;
};

void* AllocateGuardNear(HANDLE process, std::uint64_t target,
                        std::size_t size);
void* AllocateGuardInRange(HANDLE process, std::uint64_t lower,
                           std::uint64_t upper, std::size_t size);

template <std::size_t N>
std::size_t CountExactBytes(const std::uint8_t* bytes, std::size_t size,
                            const std::array<std::uint8_t, N>& pattern) {
    if (!bytes || size < N)
        return 0;
    std::size_t matches = 0;
    for (std::size_t offset = 0; offset <= size - N; ++offset) {
        if (std::equal(pattern.begin(), pattern.end(), bytes + offset))
            ++matches;
    }
    return matches;
}

bool ModelRenderCallsiteMatches(const std::uint8_t* bytes,
                                std::size_t available) {
    if (!bytes || available < kModelRenderCallFingerprint.size())
        return false;
    for (std::size_t index = 0;
         index != kModelRenderCallFingerprint.size(); ++index) {
        if (index >= kModelRenderCallOffset &&
            index < kModelRenderCallOffset +
                        kModelRenderOriginalCall.size())
            continue;
        if (bytes[index] != kModelRenderCallFingerprint[index])
            return false;
    }
    return true;
}

bool ResolveModelRenderGuardTarget(HANDLE process,
                                   const ModuleInfo& studiorender,
                                   ModelRenderGuardState& state,
                                   Failure& failure) {
    std::vector<ExecutableSection> sections;
    if (!LoadExecutableSections(process, studiorender, sections, failure, 0))
        return false;
    std::size_t matches = 0;
    std::uint64_t selected_call = 0;
    std::uint64_t selected_callee = 0;
    std::uint64_t selected_allocation = 0;
    std::array<std::uint8_t, kModelRenderOriginalCall.size()>
        selected_call_bytes{};
    std::array<std::uint8_t, kModelRenderGuardCode.size()> selected_code{};
    bool selected_installed = false;
    for (const auto& section : sections) {
        if (section.bytes.size() < kModelRenderCallFingerprint.size())
            continue;
        for (std::size_t offset = 0;
             offset <= section.bytes.size() -
                           kModelRenderCallFingerprint.size();
             ++offset) {
            if (!ModelRenderCallsiteMatches(
                    section.bytes.data() + offset,
                    section.bytes.size() - offset))
                continue;
            const std::uint64_t call =
                studiorender.base + section.rva + offset +
                kModelRenderCallOffset;
            std::array<std::uint8_t, kModelRenderOriginalCall.size()>
                call_bytes{};
            std::copy_n(section.bytes.begin() + offset +
                            kModelRenderCallOffset,
                        call_bytes.size(), call_bytes.begin());
            if (call_bytes[0] != 0xE8)
                continue;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, call_bytes.data() + 1,
                        sizeof(displacement));
            if (call > INT64_MAX - kModelRenderOriginalCall.size())
                continue;
            const std::int64_t destination_signed =
                static_cast<std::int64_t>(
                    call + kModelRenderOriginalCall.size()) +
                displacement;
            if (destination_signed <= 0)
                continue;
            const auto destination =
                static_cast<std::uint64_t>(destination_signed);
            std::uint64_t callee = destination;
            std::uint64_t allocation = 0;
            std::array<std::uint8_t, kModelRenderGuardCode.size()> code{};
            bool installed = false;
            if (!RangeInModule(destination, 1, studiorender)) {
                MEMORY_BASIC_INFORMATION memory{};
                if (!ReadExact(process, destination, code.data(),
                               code.size()) ||
                    VirtualQueryEx(
                        process,
                        reinterpret_cast<const void*>(destination),
                        &memory, sizeof(memory)) != sizeof(memory) ||
                    memory.State != MEM_COMMIT ||
                    memory.Type != MEM_PRIVATE ||
                    memory.Protect != PAGE_EXECUTE_READ ||
                    reinterpret_cast<std::uint64_t>(
                        memory.AllocationBase) != destination)
                    continue;
                std::memcpy(&callee, code.data() + 0x30,
                            sizeof(callee));
                auto expected_code = kModelRenderGuardCode;
                std::memcpy(expected_code.data() + 0x30, &callee,
                            sizeof(callee));
                if (code != expected_code)
                    continue;
                allocation = destination;
                installed = true;
            } else if (call_bytes != kModelRenderOriginalCall) {
                continue;
            }
            std::array<std::uint8_t, kModelRenderCalleePrefix.size()> prefix{};
            std::array<std::uint8_t, 0x180> body{};
            if (!RangeInModule(callee, body.size(), studiorender) ||
                !ExecutableRange(process, callee, body.size()) ||
                !ReadExact(process, callee, body.data(), body.size()))
                continue;
            std::copy_n(body.begin(), prefix.size(), prefix.begin());
            if (prefix != kModelRenderCalleePrefix)
                continue;
            const std::size_t fault_shapes =
                CountExactBytes(body.data(), body.size(),
                                kModelRenderFaultShape) +
                CountExactBytes(body.data(), body.size(),
                                kModelRenderLegacyFaultShape);
            if (fault_shapes != 1)
                continue;
            ++matches;
            selected_call = call;
            selected_callee = callee;
            selected_allocation = allocation;
            selected_call_bytes = call_bytes;
            selected_code = code;
            selected_installed = installed;
        }
    }
    if (matches != 1)
        return Fail(failure, "model_render_guard", "callsite_unique", 0,
                    studiorender.base, matches, 1);
    std::array<std::uint8_t, kModelRenderCallFingerprint.size()>
        callsite{};
    if (!ReadExact(process,
                   selected_call - kModelRenderCallOffset,
                   callsite.data(), callsite.size()) ||
        !ModelRenderCallsiteMatches(callsite.data(), callsite.size()))
        return Fail(failure, "model_render_guard", "callsite_exact", 0,
                    selected_call, 0, 1, GetLastError());
    if (!VerifyBytes(process, selected_callee,
                     kModelRenderCalleePrefix.data(),
                     kModelRenderCalleePrefix.size(), failure,
                     "model_render_guard", "callee_exact", 0))
        return false;
    state.target = selected_call;
    state.original_target = selected_callee;
    if (selected_installed) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(
                process, reinterpret_cast<const void*>(state.target),
                &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return Fail(failure, "model_render_guard",
                        "target_protection", 0, state.target,
                        memory.Protect, 1, GetLastError());
        state.allocation = selected_allocation;
        state.code = selected_code;
        state.replacement = selected_call_bytes;
        state.protection = memory.Protect;
        state.installed = true;
    } else if (!VerifyBytes(process, selected_call,
                            kModelRenderOriginalCall.data(),
                            kModelRenderOriginalCall.size(), failure,
                            "model_render_guard", "call_exact", 0)) {
        return false;
    }
    return true;
}

bool VerifyModelRenderGuard(HANDLE process,
                            const ModelRenderGuardState& state,
                            Failure& failure);

bool InstallModelRenderGuard(HANDLE process,
                             const ModuleInfo& studiorender,
                             ModelRenderGuardState& state,
                             Failure& failure) {
    if (!studiorender.base ||
        !ResolveModelRenderGuardTarget(process, studiorender, state, failure))
        return false;
    if (state.installed)
        return VerifyModelRenderGuard(process, state, failure);
    void* allocation = AllocateGuardNear(process, state.target, 0x1000);
    if (!allocation)
        return Fail(failure, "model_render_guard", "allocate_near", 0,
                    state.target, 0, 0x1000, GetLastError());
    state.allocation = reinterpret_cast<std::uint64_t>(allocation);
    state.allocation_owned = true;
    state.code = kModelRenderGuardCode;
    std::memcpy(state.code.data() + 0x30, &state.original_target,
                sizeof(state.original_target));
    if (!WriteExact(process, state.allocation, state.code.data(),
                    state.code.size()))
        return Fail(failure, "model_render_guard", "code_write", 0,
                    state.allocation, 0, state.code.size(), GetLastError());
    DWORD code_protection = 0;
    if (!VirtualProtectEx(process, allocation, 0x1000, PAGE_EXECUTE_READ,
                          &code_protection) ||
        !FlushInstructionCache(process, allocation, state.code.size()))
        return Fail(failure, "model_render_guard", "code_rx", 0,
                    state.allocation, 0, PAGE_EXECUTE_READ, GetLastError());
    const std::int64_t replacement_displacement =
        static_cast<std::int64_t>(state.allocation) -
        static_cast<std::int64_t>(state.target + state.replacement.size());
    if (replacement_displacement < INT32_MIN ||
        replacement_displacement > INT32_MAX)
        return Fail(failure, "model_render_guard", "call_reach", 0,
                    state.target, state.allocation, INT32_MAX);
    state.replacement[0] = 0xE8;
    const auto displacement =
        static_cast<std::int32_t>(replacement_displacement);
    std::memcpy(state.replacement.data() + 1, &displacement,
                sizeof(displacement));
    DWORD previous = 0;
    if (!VirtualProtectEx(process,
                          reinterpret_cast<void*>(state.target),
                          state.replacement.size(), PAGE_EXECUTE_READWRITE,
                          &previous))
        return Fail(failure, "model_render_guard", "call_protect", 0,
                    state.target, 0, PAGE_EXECUTE_READWRITE, GetLastError());
    state.protection = previous;
    state.protection_changed = true;
    state.target_may_be_changed = true;
    const bool wrote =
        WriteExact(process, state.target, state.replacement.data(),
                   state.replacement.size());
    const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
    const bool flushed =
        wrote && FlushInstructionCache(
                     process, reinterpret_cast<void*>(state.target),
                     state.replacement.size());
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(state.target),
                         state.replacement.size(), state.protection,
                         &ignored) != FALSE;
    if (restored)
        state.protection_changed = false;
    if (!wrote || !flushed || !restored)
        return Fail(failure, "model_render_guard", "call_patch", 0,
                    state.target, wrote | (static_cast<std::uint64_t>(flushed)
                                           << 1) |
                                      (static_cast<std::uint64_t>(restored)
                                           << 2),
                    7, !wrote ? write_error
                              : !flushed ? flush_error : GetLastError());
    state.changed = true;
    return VerifyModelRenderGuard(process, state, failure);
}

bool VerifyModelRenderGuard(HANDLE process,
                            const ModelRenderGuardState& state,
                            Failure& failure) {
    std::array<std::uint8_t, kModelRenderOriginalCall.size()> call{};
    std::array<std::uint8_t, kModelRenderGuardCode.size()> code{};
    if ((!state.changed && !state.installed) || !state.target ||
        !state.original_target ||
        !state.allocation || !ExecutableRange(process, state.target,
                                               call.size()) ||
        !ExecutableRange(process, state.allocation, code.size()) ||
        !ReadExact(process, state.target, call.data(), call.size()) ||
        !ReadExact(process, state.allocation, code.data(), code.size()) ||
        call != state.replacement || code != state.code)
        return Fail(failure, "model_render_guard", "post_exact", 0,
                    state.target, state.allocation, 1, GetLastError());
    MEMORY_BASIC_INFORMATION stub_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &stub_memory, sizeof(stub_memory)) !=
            sizeof(stub_memory) ||
        stub_memory.State != MEM_COMMIT ||
        stub_memory.Type != MEM_PRIVATE ||
        stub_memory.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<std::uint64_t>(stub_memory.AllocationBase) !=
            state.allocation)
        return Fail(failure, "model_render_guard", "stub_rx_exact", 0,
                    state.allocation, stub_memory.Protect,
                    PAGE_EXECUTE_READ, GetLastError());
    MEMORY_BASIC_INFORMATION target_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.target),
                       &target_memory, sizeof(target_memory)) !=
            sizeof(target_memory) ||
        target_memory.State != MEM_COMMIT ||
        target_memory.Protect != state.protection ||
        (target_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return Fail(failure, "model_render_guard",
                    "target_protection_exact", 0, state.target,
                    target_memory.Protect, state.protection,
                    GetLastError());
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
    const std::int64_t destination =
        static_cast<std::int64_t>(state.target + call.size()) + displacement;
    std::uint64_t embedded_target = 0;
    std::memcpy(&embedded_target, code.data() + 0x30,
                sizeof(embedded_target));
    if (call[0] != 0xE8 || destination <= 0 ||
        static_cast<std::uint64_t>(destination) != state.allocation ||
        embedded_target != state.original_target)
        return Fail(failure, "model_render_guard", "post_flow", 0,
                    state.target, destination, state.allocation);
    return true;
}

bool RollbackModelRenderGuard(HANDLE process,
                              ModelRenderGuardState& state) {
    const bool ever_published = state.target_may_be_changed;
    if (state.target_may_be_changed) {
        DWORD previous = 0;
        bool protected_for_write = state.protection_changed;
        if (!protected_for_write)
            protected_for_write =
                VirtualProtectEx(process,
                                 reinterpret_cast<void*>(state.target),
                                 kModelRenderOriginalCall.size(),
                                 PAGE_EXECUTE_READWRITE, &previous) != FALSE;
        const bool wrote =
            protected_for_write &&
            WriteExact(process, state.target,
                       kModelRenderOriginalCall.data(),
                       kModelRenderOriginalCall.size());
        const bool flushed =
            wrote && FlushInstructionCache(
                         process, reinterpret_cast<void*>(state.target),
                         kModelRenderOriginalCall.size()) != FALSE;
        DWORD ignored = 0;
        const bool restored =
            protected_for_write &&
            VirtualProtectEx(process,
                             reinterpret_cast<void*>(state.target),
                             kModelRenderOriginalCall.size(),
                             state.protection, &ignored) != FALSE;
        std::array<std::uint8_t, kModelRenderOriginalCall.size()> observed{};
        MEMORY_BASIC_INFORMATION target_memory{};
        const bool exact =
            ReadExact(process, state.target, observed.data(),
                      observed.size()) &&
            observed == kModelRenderOriginalCall;
        const bool protection_exact =
            VirtualQueryEx(process,
                           reinterpret_cast<const void*>(state.target),
                           &target_memory, sizeof(target_memory)) ==
                sizeof(target_memory) &&
            target_memory.State == MEM_COMMIT &&
            target_memory.Protect == state.protection &&
            (target_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (!protected_for_write || !wrote || !flushed || !restored ||
            !exact || !protection_exact)
            return false;
        state.target_may_be_changed = false;
        state.protection_changed = false;
        state.changed = false;
    }
    if (ever_published)
        return true;
    if (state.allocation_owned && state.allocation &&
        !VirtualFreeEx(process,
                       reinterpret_cast<void*>(state.allocation), 0,
                       MEM_RELEASE))
        return false;
    state = {};
    return true;
}

constexpr std::size_t kPredictionGuardCodeSize = 0x1CF;
constexpr std::size_t kPredictionGuardPayloadSize = 0x360;
constexpr std::size_t kPredictionGuardContextOffset = 0x1000;
constexpr std::size_t kPredictionGuardAllocationSize = 0x2000;
constexpr std::size_t kPredictionGuardFunctionTableOffset = 0x300;
constexpr std::array<std::uint32_t, 3> kPredictionGuardCallRvas{{
    0x2FC7DF, 0x2FC9FD, 0x2FCC26,
}};
constexpr std::array<std::uint32_t, 3> kPredictionGuardOriginalRvas{{
    0x349A30, 0x3496C0, 0x3498E0,
}};
constexpr std::array<std::uint32_t, 3> kPredictionGuardWrapperOffsets{{
    0x000, 0x040, 0x090,
}};
constexpr std::array<std::uint32_t, 3> kPredictionGuardTailJumpOffsets{{
    0x01E, 0x068, 0x0AF,
}};
constexpr std::array<std::array<std::uint8_t, 5>, 3>
    kPredictionGuardOriginalCalls{{
        {{0xE8, 0x4C, 0xD2, 0x04, 0x00}},
        {{0xE8, 0xBE, 0xCC, 0x04, 0x00}},
        {{0xE8, 0xB5, 0xCC, 0x04, 0x00}},
    }};
constexpr std::array<std::array<std::uint8_t, 16>, 3>
    kPredictionGuardCalleePrefixes{{
        {{0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0x0D, 0xA5,
          0x0D, 0x4A, 0x00, 0x41, 0x8B, 0x11, 0x45, 0x8B}},
        {{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
          0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C}},
        {{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
          0xEC, 0x40, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x48}},
    }};
constexpr std::array<std::uint32_t, 8> kPredictionGuardRequiredSlots{{
    0x7EA8A8, 0x7EA7E0, 0x7EA7D8, 0x7EA8B8,
    0x7EA828, 0x7EA868, 0x7EA860, 0x7EA8A0,
}};
constexpr std::array<std::uint64_t, 8> kPredictionGuardSlotMarkers{{
    0x4444444444444444ull, 0x5555555555555555ull,
    0x6666666666666666ull, 0x7777777777777777ull,
    0x8888888888888888ull, 0x9999999999999999ull,
    0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull,
}};
constexpr std::uint64_t kPredictionGuardContextMagic =
    0x4452474752505353ull;

struct PredictionGuardContext {
    std::uint64_t magic = kPredictionGuardContextMagic;
    std::uint64_t code = 0;
    std::uint64_t fix_suppressed = 0;
    std::uint64_t fix_ready = 0;
    std::uint64_t start_suppressed = 0;
    std::uint64_t start_ready = 0;
    std::uint64_t finish_suppressed = 0;
    std::uint64_t finish_ready = 0;
};

static_assert(offsetof(PredictionGuardContext, fix_suppressed) == 0x10);
static_assert(offsetof(PredictionGuardContext, finish_ready) == 0x38);
static_assert(sizeof(PredictionGuardContext) == 0x40);

constexpr std::array<std::uint64_t, 7> kPredictionGuardCounterMarkers{{
    0xC0C0C0C0C0C0C0C0ull, 0xC1C1C1C1C1C1C1C1ull,
    0xC2C2C2C2C2C2C2C2ull, 0xC3C3C3C3C3C3C3C3ull,
    0xC4C4C4C4C4C4C4C4ull, 0xC5C5C5C5C5C5C5C5ull,
    0xC6C6C6C6C6C6C6C6ull,
}};
constexpr std::array<std::size_t, 7> kPredictionGuardCounterOffsets{{
    offsetof(PredictionGuardContext, fix_suppressed),
    offsetof(PredictionGuardContext, fix_ready),
    offsetof(PredictionGuardContext, start_suppressed),
    offsetof(PredictionGuardContext, start_ready),
    offsetof(PredictionGuardContext, finish_suppressed),
    offsetof(PredictionGuardContext, finish_ready),
    offsetof(PredictionGuardContext, start_ready),
}};

constexpr auto kPredictionGuardCodeTemplate =
    std::to_array<std::uint8_t>({
        0x48, 0x83, 0xEC, 0x28, 0xE8, 0xC7, 0x00, 0x00,
        0x00, 0x48, 0x83, 0xC4, 0x28, 0x84, 0xC0, 0x74,
        0x19, 0x49, 0xBB, 0xC1, 0xC1, 0xC1, 0xC1, 0xC1,
        0xC1, 0xC1, 0xC1, 0x49, 0xFF, 0x03, 0xE9, 0x00,
        0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x49, 0xBB, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0x49, 0xFF, 0x03, 0xC3,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0x48, 0x89, 0x54, 0x24, 0x08, 0x48, 0x83, 0xEC,
        0x28, 0xE8, 0x82, 0x00, 0x00, 0x00, 0x48, 0x83,
        0xC4, 0x28, 0x84, 0xC0, 0x74, 0x1E, 0x49, 0xBB,
        0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3,
        0x49, 0xFF, 0x03, 0x48, 0x8B, 0x54, 0x24, 0x08,
        0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x49, 0xBB, 0xC2, 0xC2,
        0xC2, 0xC2, 0xC2, 0xC2, 0xC2, 0xC2, 0x49, 0xFF,
        0x03, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0x49, 0xBB, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6,
        0xC6, 0xC6, 0x49, 0xBA, 0xC5, 0xC5, 0xC5, 0xC5,
        0xC5, 0xC5, 0xC5, 0xC5, 0x49, 0x8B, 0x03, 0x49,
        0x3B, 0x02, 0x76, 0x0C, 0x49, 0xFF, 0x02, 0xE9,
        0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90,
        0x49, 0xBB, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4,
        0xC4, 0xC4, 0x49, 0xFF, 0x03, 0xC3, 0xCC, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0x48, 0x83, 0xEC, 0x38, 0x48, 0xB8, 0x44, 0x44,
        0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x48, 0x8B,
        0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xDE, 0x00,
        0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x28, 0x48,
        0xB8, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
        0x55, 0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0, 0x0F,
        0x84, 0xC3, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0xB8,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84,
        0xA4, 0x00, 0x00, 0x00, 0x48, 0xB8, 0x77, 0x77,
        0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x48, 0x8B,
        0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x8E, 0x00,
        0x00, 0x00, 0x48, 0xB8, 0x88, 0x88, 0x88, 0x88,
        0x88, 0x88, 0x88, 0x88, 0x48, 0x8B, 0x00, 0x48,
        0x85, 0xC0, 0x74, 0x7C, 0x48, 0xB8, 0x99, 0x99,
        0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x48, 0x8B,
        0x00, 0x48, 0x85, 0xC0, 0x74, 0x6A, 0x48, 0xB8,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x58,
        0x48, 0xB8, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
        0xBB, 0xBB, 0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0,
        0x74, 0x46, 0x48, 0x8B, 0x4C, 0x24, 0x28, 0x48,
        0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x39, 0x4C,
        0x8B, 0x98, 0xD8, 0x00, 0x00, 0x00, 0x4D, 0x85,
        0xDB, 0x74, 0x2D, 0x41, 0xFF, 0xD3, 0x84, 0xC0,
        0x74, 0x26, 0x48, 0x8B, 0x4C, 0x24, 0x28, 0x48,
        0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x19, 0x4C,
        0x8B, 0x98, 0xD0, 0x00, 0x00, 0x00, 0x4D, 0x85,
        0xDB, 0x74, 0x0D, 0x41, 0xFF, 0xD3, 0x84, 0xC0,
        0x0F, 0x95, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3,
        0x31, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3,
    });

static_assert(kPredictionGuardCodeTemplate.size() ==
              kPredictionGuardCodeSize);
static_assert(kPredictionGuardCodeTemplate[0x004] == 0xE8 &&
              kPredictionGuardCodeTemplate[0x005] == 0xC7 &&
              kPredictionGuardCodeTemplate[0x009] == 0x48 &&
              0x009 + 0xC7 == 0x0D0);
static_assert(kPredictionGuardCodeTemplate[0x049] == 0xE8 &&
              kPredictionGuardCodeTemplate[0x04A] == 0x82 &&
              kPredictionGuardCodeTemplate[0x04E] == 0x48 &&
              0x04E + 0x82 == 0x0D0);
static_assert(kPredictionGuardCodeTemplate[0x0C8] == 0xCC &&
              kPredictionGuardCodeTemplate[0x0CF] == 0xCC &&
              kPredictionGuardCodeTemplate[0x0D0] == 0x48 &&
              kPredictionGuardCodeTemplate[0x0D1] == 0x83 &&
              kPredictionGuardCodeTemplate[0x0D2] == 0xEC &&
              kPredictionGuardCodeTemplate[0x0D3] == 0x38);
static_assert(kPredictionGuardCodeTemplate[0x0EF] == 0x48 &&
              kPredictionGuardCodeTemplate[0x0F0] == 0xB8 &&
              kPredictionGuardCodeTemplate[0x0F9] == 0x48 &&
              kPredictionGuardCodeTemplate[0x0FA] == 0x8B &&
              kPredictionGuardCodeTemplate[0x0FB] == 0x00 &&
              kPredictionGuardCodeTemplate[0x0FC] == 0x48 &&
              kPredictionGuardCodeTemplate[0x0FF] == 0x0F &&
              kPredictionGuardCodeTemplate[0x104] == 0x00);
static_assert(kPredictionGuardCodeTemplate[0x105] == 0x90 &&
              kPredictionGuardCodeTemplate[0x106] == 0x90 &&
              kPredictionGuardCodeTemplate[0x107] == 0x90 &&
              kPredictionGuardCodeTemplate[0x108] == 0x90 &&
              kPredictionGuardCodeTemplate[0x109] == 0x90 &&
              kPredictionGuardCodeTemplate[0x10A] == 0x90 &&
              kPredictionGuardCodeTemplate[0x10B] == 0x90 &&
              kPredictionGuardCodeTemplate[0x10C] == 0x90 &&
              kPredictionGuardCodeTemplate[0x10D] == 0x90 &&
              kPredictionGuardCodeTemplate[0x10E] == 0x48 &&
              kPredictionGuardCodeTemplate[0x10F] == 0xB8);
static_assert(kPredictionGuardCodeTemplate[0x19B] == 0x41 &&
              kPredictionGuardCodeTemplate[0x19C] == 0xFF &&
              kPredictionGuardCodeTemplate[0x19D] == 0xD3 &&
              kPredictionGuardCodeTemplate[0x1BB] == 0x41 &&
              kPredictionGuardCodeTemplate[0x1BC] == 0xFF &&
              kPredictionGuardCodeTemplate[0x1BD] == 0xD3 &&
              kPredictionGuardCodeTemplate[0x1BE] == 0x84 &&
              kPredictionGuardCodeTemplate[0x1BF] == 0xC0 &&
              kPredictionGuardCodeTemplate[0x1C0] == 0x0F &&
              kPredictionGuardCodeTemplate[0x1C1] == 0x95 &&
              kPredictionGuardCodeTemplate[0x1C2] == 0xC0);

struct PredictionGuardState {
    std::array<std::uint64_t, 3> targets{};
    std::array<std::uint64_t, 3> originals{};
    std::uint64_t allocation_lower = 0;
    std::uint64_t allocation_upper = 0;
    std::uint64_t allocation = 0;
    std::uint64_t context = 0;
    std::array<std::uint8_t, kPredictionGuardPayloadSize> payload{};
    std::array<std::array<std::uint8_t, 5>, 3> replacements{};
    std::array<DWORD, 3> protections{};
    std::array<bool, 3> protection_changed{};
    std::array<bool, 3> target_may_be_changed{};
    std::array<bool, 3> target_installed{};
    bool prepared = false;
    bool function_table_registered = false;
    bool function_table_owned = false;
    bool allocation_owned = false;
    bool applied = false;
};

bool ComputePredictionGuardAllocationRange(
    const PredictionGuardState& state, std::uint64_t& lower,
    std::uint64_t& upper, Failure& failure) {
    std::int64_t intersection_lower = 0;
    std::int64_t intersection_upper = INT64_MAX;
    for (std::size_t index = 0; index != state.targets.size(); ++index) {
        if (state.targets[index] >
                static_cast<std::uint64_t>(INT64_MAX) - 5 ||
            state.originals[index] > static_cast<std::uint64_t>(INT64_MAX))
            return Fail(failure, "prediction_guard",
                        "rel32_address_signed", index,
                        state.targets[index], state.originals[index],
                        INT64_MAX);
        const std::int64_t call_after =
            static_cast<std::int64_t>(state.targets[index] + 5);
        const std::int64_t original =
            static_cast<std::int64_t>(state.originals[index]);
        const std::int64_t wrapper_offset =
            static_cast<std::int64_t>(
                kPredictionGuardWrapperOffsets[index]);
        const std::int64_t tail_after_offset =
            static_cast<std::int64_t>(
                kPredictionGuardTailJumpOffsets[index] + 5);
        const std::int64_t call_lower =
            call_after + static_cast<std::int64_t>(INT32_MIN) -
            wrapper_offset;
        const std::int64_t call_upper =
            call_after + static_cast<std::int64_t>(INT32_MAX) -
            wrapper_offset;
        const std::int64_t tail_lower =
            original - tail_after_offset -
            static_cast<std::int64_t>(INT32_MAX);
        const std::int64_t tail_upper =
            original - tail_after_offset -
            static_cast<std::int64_t>(INT32_MIN);
        intersection_lower =
            std::max(intersection_lower,
                     std::max(call_lower, tail_lower));
        intersection_upper =
            std::min(intersection_upper,
                     std::min(call_upper, tail_upper));
    }
    if (intersection_lower < 0 ||
        intersection_lower > intersection_upper)
        return Fail(failure, "prediction_guard", "rel32_intersection", 0,
                    static_cast<std::uint64_t>(intersection_lower),
                    static_cast<std::uint64_t>(intersection_upper), 1);
    lower = static_cast<std::uint64_t>(intersection_lower);
    upper = static_cast<std::uint64_t>(intersection_upper);
    return true;
}

bool PatchPredictionGuardQword(
    std::array<std::uint8_t, kPredictionGuardPayloadSize>& payload,
    std::uint64_t marker, std::uint64_t value) {
    std::size_t matches = 0;
    std::size_t selected = 0;
    for (std::size_t offset = 0;
         offset + sizeof(marker) <= kPredictionGuardCodeSize; ++offset) {
        std::uint64_t observed = 0;
        std::memcpy(&observed, payload.data() + offset, sizeof(observed));
        if (observed == marker) {
            selected = offset;
            ++matches;
        }
    }
    if (matches != 1)
        return false;
    std::memcpy(payload.data() + selected, &value, sizeof(value));
    return true;
}

bool BuildPredictionGuardPayload(std::uint64_t image_base,
                                 PredictionGuardState& state,
                                 Failure& failure);

bool ValidatePredictionGuardFunctionTable(
    HANDLE process, const PredictionGuardState& state,
    Failure& failure);

bool BuildPredictionGuardReplacements(PredictionGuardState& state,
                                      Failure& failure) {
    for (std::size_t index = 0; index != state.targets.size(); ++index) {
        const std::uint64_t wrapper =
            state.allocation + kPredictionGuardWrapperOffsets[index];
        const std::int64_t relative =
            static_cast<std::int64_t>(wrapper) -
            static_cast<std::int64_t>(state.targets[index] + 5);
        if (relative < INT32_MIN || relative > INT32_MAX)
            return Fail(failure, "prediction_guard", "call_reach", index,
                        state.targets[index], wrapper, INT32_MAX);
        state.replacements[index][0] = 0xE8;
        const std::int32_t displacement =
            static_cast<std::int32_t>(relative);
        std::memcpy(state.replacements[index].data() + 1, &displacement,
                    sizeof(displacement));
    }
    return true;
}

bool PreparePredictionGuard(HANDLE process, std::uint64_t image_base,
                            PredictionGuardState& state,
                            Failure& failure) {
    std::array<std::array<std::uint8_t, 5>, 3> live_calls{};
    std::uint64_t recovered_allocation = 0;
    for (std::size_t index = 0; index != state.targets.size(); ++index) {
        state.targets[index] = image_base + kPredictionGuardCallRvas[index];
        state.originals[index] =
            image_base + kPredictionGuardOriginalRvas[index];
        if (!ReadExact(process, state.targets[index],
                       live_calls[index].data(),
                       live_calls[index].size()) ||
            !VerifyBytes(process, state.originals[index],
                         kPredictionGuardCalleePrefixes[index].data(),
                         kPredictionGuardCalleePrefixes[index].size(),
                         failure, "prediction_guard", "callee_exact", index))
            return false;
        std::int32_t displacement = 0;
        std::memcpy(&displacement,
                    kPredictionGuardOriginalCalls[index].data() + 1,
                    sizeof(displacement));
        const std::int64_t destination =
            static_cast<std::int64_t>(state.targets[index] + 5) +
            displacement;
        if (destination != static_cast<std::int64_t>(state.originals[index]))
            return Fail(failure, "prediction_guard", "call_destination",
                        index, state.targets[index], destination,
                        state.originals[index]);
        if (live_calls[index] == kPredictionGuardOriginalCalls[index])
            continue;
        if (live_calls[index][0] != 0xE8)
            return Fail(failure, "prediction_guard", "call_known_state",
                        index, state.targets[index],
                        PrefixValue(live_calls[index].data(),
                                    live_calls[index].size()),
                        PrefixValue(
                            kPredictionGuardOriginalCalls[index].data(),
                            kPredictionGuardOriginalCalls[index].size()));
        std::int32_t live_displacement = 0;
        std::memcpy(&live_displacement,
                    live_calls[index].data() + 1,
                    sizeof(live_displacement));
        const std::int64_t wrapper_signed =
            static_cast<std::int64_t>(state.targets[index] + 5) +
            live_displacement;
        if (wrapper_signed <=
            static_cast<std::int64_t>(
                kPredictionGuardWrapperOffsets[index]))
            return Fail(failure, "prediction_guard",
                        "wrapper_positive", index, state.targets[index],
                        static_cast<std::uint64_t>(wrapper_signed),
                        kPredictionGuardWrapperOffsets[index] + 1);
        const std::uint64_t candidate =
            static_cast<std::uint64_t>(wrapper_signed) -
            kPredictionGuardWrapperOffsets[index];
        if (recovered_allocation &&
            recovered_allocation != candidate)
            return Fail(failure, "prediction_guard",
                        "allocation_consistent", index,
                        state.targets[index], candidate,
                        recovered_allocation);
        recovered_allocation = candidate;
        state.target_installed[index] = true;
    }
    for (std::size_t index = 0;
         index != kPredictionGuardRequiredSlots.size(); ++index) {
        const std::uint64_t slot =
            image_base + kPredictionGuardRequiredSlots[index];
        if (!ReadableRange(process, slot, sizeof(std::uint64_t)))
            return Fail(failure, "prediction_guard", "slot_readable",
                        index, slot, 0, sizeof(std::uint64_t),
                        GetLastError());
    }
    if (!ComputePredictionGuardAllocationRange(
            state, state.allocation_lower, state.allocation_upper,
            failure))
        return false;
    if (recovered_allocation) {
        if (recovered_allocation < state.allocation_lower ||
            recovered_allocation > state.allocation_upper)
            return Fail(failure, "prediction_guard",
                        "allocation_in_range", 0, recovered_allocation,
                        state.allocation_lower, state.allocation_upper);
        state.allocation = recovered_allocation;
        state.context =
            state.allocation + kPredictionGuardContextOffset;
        if (!BuildPredictionGuardPayload(image_base, state, failure) ||
            !BuildPredictionGuardReplacements(state, failure))
            return false;
        for (std::size_t index = 0;
             index != state.targets.size(); ++index) {
            if (state.target_installed[index] &&
                live_calls[index] != state.replacements[index])
                return Fail(failure, "prediction_guard",
                            "call_replacement_exact", index,
                            state.targets[index],
                            PrefixValue(live_calls[index].data(),
                                        live_calls[index].size()),
                            PrefixValue(state.replacements[index].data(),
                                        state.replacements[index].size()));
            MEMORY_BASIC_INFORMATION target_memory{};
            if (VirtualQueryEx(
                    process,
                    reinterpret_cast<const void*>(
                        state.targets[index]),
                    &target_memory, sizeof(target_memory)) !=
                    sizeof(target_memory) ||
                target_memory.State != MEM_COMMIT ||
                (target_memory.Protect &
                 (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return Fail(failure, "prediction_guard",
                            "target_protection", index,
                            state.targets[index],
                            target_memory.Protect, 1,
                            GetLastError());
            state.protections[index] = target_memory.Protect;
        }
        std::array<std::uint8_t, kPredictionGuardPayloadSize> payload{};
        PredictionGuardContext context{};
        MEMORY_BASIC_INFORMATION code_memory{};
        MEMORY_BASIC_INFORMATION context_memory{};
        if (!ReadExact(process, state.allocation, payload.data(),
                       payload.size()) ||
            payload != state.payload ||
            !ReadValue(process, state.context, context) ||
            context.magic != kPredictionGuardContextMagic ||
            context.code != state.allocation ||
            context.finish_ready > context.start_ready ||
            VirtualQueryEx(
                process,
                reinterpret_cast<const void*>(state.allocation),
                &code_memory, sizeof(code_memory)) !=
                sizeof(code_memory) ||
            code_memory.State != MEM_COMMIT ||
            code_memory.Type != MEM_PRIVATE ||
            code_memory.Protect != PAGE_EXECUTE_READ ||
            reinterpret_cast<std::uint64_t>(
                code_memory.AllocationBase) != state.allocation ||
            VirtualQueryEx(
                process,
                reinterpret_cast<const void*>(state.context),
                &context_memory, sizeof(context_memory)) !=
                sizeof(context_memory) ||
            context_memory.State != MEM_COMMIT ||
            context_memory.Type != MEM_PRIVATE ||
            context_memory.Protect != PAGE_READWRITE ||
            reinterpret_cast<std::uint64_t>(
                context_memory.AllocationBase) != state.allocation)
            return Fail(failure, "prediction_guard",
                        "installed_state_exact", 0,
                        state.allocation, context.magic,
                        kPredictionGuardContextMagic,
                        GetLastError());
        if (!ValidatePredictionGuardFunctionTable(
                process, state, failure))
            return false;
        state.function_table_registered = true;
        state.applied = std::all_of(
            state.target_installed.begin(),
            state.target_installed.end(),
            [](bool value) { return value; });
    }
    state.prepared = true;
    return true;
}

bool BuildPredictionGuardPayload(std::uint64_t image_base,
                                 PredictionGuardState& state,
                                 Failure& failure) {
    state.payload.fill(0);
    std::copy(kPredictionGuardCodeTemplate.begin(),
              kPredictionGuardCodeTemplate.end(), state.payload.begin());
    for (std::size_t index = 0;
         index != kPredictionGuardSlotMarkers.size(); ++index) {
        if (!PatchPredictionGuardQword(
                state.payload, kPredictionGuardSlotMarkers[index],
                image_base + kPredictionGuardRequiredSlots[index]))
            return Fail(failure, "prediction_guard", "slot_embed_unique",
                        index, state.allocation,
                        kPredictionGuardSlotMarkers[index], 1);
    }
    for (std::size_t index = 0;
         index != kPredictionGuardCounterMarkers.size(); ++index) {
        if (!PatchPredictionGuardQword(
                state.payload, kPredictionGuardCounterMarkers[index],
                state.context + kPredictionGuardCounterOffsets[index]))
            return Fail(failure, "prediction_guard",
                        "counter_embed_unique", index, state.allocation,
                        kPredictionGuardCounterMarkers[index], 1);
    }
    for (std::size_t index = 0;
         index != kPredictionGuardTailJumpOffsets.size(); ++index) {
        const std::size_t offset = kPredictionGuardTailJumpOffsets[index];
        if (state.payload[offset] != 0xE9)
            return Fail(failure, "prediction_guard", "tail_opcode", index,
                        state.allocation + offset, state.payload[offset],
                        0xE9);
        const std::int64_t relative =
            static_cast<std::int64_t>(state.originals[index]) -
            static_cast<std::int64_t>(state.allocation + offset + 5);
        if (relative < INT32_MIN || relative > INT32_MAX)
            return Fail(failure, "prediction_guard", "tail_reach", index,
                        state.allocation + offset, state.originals[index],
                        INT32_MAX);
        const std::int32_t displacement =
            static_cast<std::int32_t>(relative);
        std::memcpy(state.payload.data() + offset + 1, &displacement,
                    sizeof(displacement));
    }
    constexpr std::array<RUNTIME_FUNCTION, 4> functions{{
        {0x000, 0x038, 0x340},
        {0x040, 0x082, 0x348},
        {0x090, 0x0C8, 0x350},
        {0x0D0, 0x1CF, 0x358},
    }};
    constexpr std::array<std::array<std::uint8_t, 8>, 4> unwind{{
        {{0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00}},
        {{0x01, 0x09, 0x01, 0x00, 0x09, 0x42, 0x00, 0x00}},
        {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {{0x01, 0x04, 0x01, 0x00, 0x04, 0x62, 0x00, 0x00}},
    }};
    std::memcpy(state.payload.data() + kPredictionGuardFunctionTableOffset,
                functions.data(), sizeof(functions));
    for (std::size_t index = 0; index != unwind.size(); ++index)
        std::copy(unwind[index].begin(), unwind[index].end(),
                  state.payload.begin() + 0x340 + index * 8);
    return true;
}

bool ValidatePredictionGuardFunctionTable(
    HANDLE process, const PredictionGuardState& state,
    Failure& failure) {
    const ModuleInfo ntdll = FindModule(GetProcessId(process), "ntdll.dll");
    const std::uint64_t lookup_rva =
        ExportRva(ntdll, "RtlLookupFunctionEntry");
    if (!ntdll.base || !lookup_rva)
        return Fail(failure, "prediction_guard_unwind",
                    "lookup_export", 0, ntdll.base, lookup_rva, 1);
    void* scratch = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!scratch)
        return Fail(failure, "prediction_guard_unwind",
                    "lookup_allocate", 0, state.allocation, 0,
                    0x1000, GetLastError());
    const std::uint64_t scratch_address =
        reinterpret_cast<std::uint64_t>(scratch);
    const std::uint64_t zero = 0;
    if (!WriteExact(process, scratch_address, &zero, sizeof(zero))) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, scratch, 0, MEM_RELEASE);
        return Fail(failure, "prediction_guard_unwind",
                    "lookup_initialize", 0, scratch_address, 0, 0,
                    error);
    }
    std::uint64_t result = 0;
    const bool called = RemoteCall3(
        process, ntdll.base + lookup_rva, state.allocation + 1,
        scratch_address, 0, result, failure,
        "prediction_guard_unwind_lookup", 0);
    const bool quiescent = RemoteCallsRollbackSafe();
    std::uint64_t image_base = 0;
    const bool read = quiescent &&
        ReadValue(process, scratch_address, image_base);
    const DWORD read_error = read ? ERROR_SUCCESS : GetLastError();
    const bool freed =
        !quiescent ||
        VirtualFreeEx(process, scratch, 0, MEM_RELEASE) != FALSE;
    if (!called || !quiescent)
        return false;
    if (!read || !freed)
        return Fail(failure, "prediction_guard_unwind",
                    "lookup_result_read", 0, scratch_address,
                    static_cast<std::uint64_t>(read),
                    static_cast<std::uint64_t>(freed),
                    !read ? read_error : GetLastError());
    const std::uint64_t expected =
        state.allocation + kPredictionGuardFunctionTableOffset;
    if (result != expected || image_base != state.allocation)
        return Fail(failure, "prediction_guard_unwind",
                    "lookup_exact", 0, state.allocation, result,
                    expected);
    return true;
}

bool VerifyPredictionGuard(HANDLE process,
                           const PredictionGuardState& state,
                           Failure& failure) {
    if (!state.prepared || !state.applied ||
        !state.function_table_registered || !state.allocation ||
        state.allocation < state.allocation_lower ||
        state.allocation > state.allocation_upper ||
        state.context != state.allocation + kPredictionGuardContextOffset)
        return Fail(failure, "prediction_guard", "state_complete", 0,
                    state.allocation, state.context, 1);
    std::array<std::uint8_t, kPredictionGuardPayloadSize> payload{};
    PredictionGuardContext context{};
    if (!ReadExact(process, state.allocation, payload.data(),
                   payload.size()) ||
        payload != state.payload ||
        !ReadValue(process, state.context, context) ||
        context.magic != kPredictionGuardContextMagic ||
        context.code != state.allocation)
        return Fail(failure, "prediction_guard", "payload_context_exact",
                    0, state.allocation, context.magic,
                    kPredictionGuardContextMagic, GetLastError());
    if (context.finish_ready > context.start_ready)
        return Fail(failure, "prediction_guard", "start_finish_order", 0,
                    state.context, context.finish_ready,
                    context.start_ready);
    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION context_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &code_memory, sizeof(code_memory)) !=
            sizeof(code_memory) ||
        code_memory.State != MEM_COMMIT ||
        code_memory.Type != MEM_PRIVATE ||
        code_memory.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<std::uint64_t>(code_memory.AllocationBase) !=
            state.allocation ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.context),
                       &context_memory, sizeof(context_memory)) !=
            sizeof(context_memory) ||
        context_memory.State != MEM_COMMIT ||
        context_memory.Type != MEM_PRIVATE ||
        context_memory.Protect != PAGE_READWRITE ||
        reinterpret_cast<std::uint64_t>(context_memory.AllocationBase) !=
            state.allocation)
        return Fail(failure, "prediction_guard", "memory_protection", 0,
                    state.allocation, code_memory.Protect,
                    PAGE_EXECUTE_READ, GetLastError());
    for (std::size_t index = 0; index != state.targets.size(); ++index) {
        std::array<std::uint8_t, 5> call{};
        MEMORY_BASIC_INFORMATION target_memory{};
        if (!ReadExact(process, state.targets[index], call.data(),
                       call.size()) ||
            call != state.replacements[index] ||
            VirtualQueryEx(
                process,
                reinterpret_cast<const void*>(state.targets[index]),
                &target_memory, sizeof(target_memory)) !=
                sizeof(target_memory) ||
            target_memory.State != MEM_COMMIT ||
            target_memory.Protect != state.protections[index])
            return Fail(failure, "prediction_guard", "call_exact", index,
                        state.targets[index],
                        PrefixValue(call.data(), call.size()),
                        PrefixValue(state.replacements[index].data(),
                                    state.replacements[index].size()),
                        GetLastError());
        std::int32_t displacement = 0;
        std::memcpy(&displacement, call.data() + 1,
                    sizeof(displacement));
        const std::int64_t destination =
            static_cast<std::int64_t>(state.targets[index] + 5) +
            displacement;
        const std::uint64_t expected =
            state.allocation + kPredictionGuardWrapperOffsets[index];
        if (call[0] != 0xE8 ||
            destination != static_cast<std::int64_t>(expected))
            return Fail(failure, "prediction_guard", "wrapper_destination",
                        index, state.targets[index], destination, expected);
        const std::size_t tail_offset =
            kPredictionGuardTailJumpOffsets[index];
        std::int32_t tail_displacement = 0;
        std::memcpy(&tail_displacement,
                    payload.data() + tail_offset + 1,
                    sizeof(tail_displacement));
        const std::int64_t tail_destination =
            static_cast<std::int64_t>(
                state.allocation + tail_offset + 5) +
            tail_displacement;
        if (payload[tail_offset] != 0xE9 ||
            tail_destination !=
                static_cast<std::int64_t>(state.originals[index]))
            return Fail(failure, "prediction_guard",
                        "tail_destination", index,
                        state.allocation + tail_offset,
                        tail_destination, state.originals[index]);
    }
    return true;
}

bool ApplyPredictionGuard(HANDLE process, std::uint64_t image_base,
                          PredictionGuardState& state,
                          Failure& failure) {
    if (!state.prepared)
        return Fail(failure, "prediction_guard", "prepared", 0,
                    image_base, 0, 1);
    if (state.applied)
        return VerifyPredictionGuard(process, state, failure);
    if (!state.allocation) {
        void* allocation = AllocateGuardInRange(
            process, state.allocation_lower, state.allocation_upper,
            kPredictionGuardAllocationSize);
        if (!allocation)
            return Fail(failure, "prediction_guard",
                        "allocate_rel32_intersection", 0,
                        state.allocation_lower, state.allocation_upper,
                        kPredictionGuardAllocationSize, GetLastError());
        state.allocation = reinterpret_cast<std::uint64_t>(allocation);
        state.allocation_owned = true;
        state.context =
            state.allocation + kPredictionGuardContextOffset;
        if (!BuildPredictionGuardPayload(image_base, state, failure))
            return false;
        PredictionGuardContext context{};
        context.code = state.allocation;
        if (!WriteExact(process, state.allocation, state.payload.data(),
                        state.payload.size()) ||
            !WriteExact(process, state.context, &context,
                        sizeof(context)))
            return Fail(failure, "prediction_guard",
                        "allocation_write", 0, state.allocation, 0,
                        state.payload.size(), GetLastError());
        DWORD previous = 0;
        if (!VirtualProtectEx(process, allocation, 0x1000,
                              PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(process, allocation,
                                   state.payload.size()))
            return Fail(failure, "prediction_guard", "allocation_rx", 0,
                        state.allocation, 0, PAGE_EXECUTE_READ,
                        GetLastError());
        const ModuleInfo ntdll =
            FindModule(GetProcessId(process), "ntdll.dll");
        const std::uint64_t add_function_table_rva =
            ExportRva(ntdll, "RtlAddFunctionTable");
        std::uint64_t registration_result = 0;
        if (!ntdll.base || !add_function_table_rva ||
            !RemoteCall3(
                process, ntdll.base + add_function_table_rva,
                state.allocation +
                    kPredictionGuardFunctionTableOffset,
                4, state.allocation, registration_result, failure,
                "prediction_guard_unwind", 0) ||
            registration_result == 0)
            return Fail(failure, "prediction_guard_unwind",
                        "registered", 0, state.allocation,
                        registration_result, 1, GetLastError());
        state.function_table_registered = true;
        state.function_table_owned = true;
    }
    if (!BuildPredictionGuardReplacements(state, failure))
        return false;
    for (std::size_t index = 0; index != state.targets.size(); ++index) {
        if (state.target_installed[index])
            continue;
        if (!VerifyBytes(process, state.targets[index],
                         kPredictionGuardOriginalCalls[index].data(),
                         kPredictionGuardOriginalCalls[index].size(),
                         failure, "prediction_guard", "call_recheck",
                         index))
            return false;
        if (!VirtualProtectEx(
                process, reinterpret_cast<void*>(state.targets[index]),
                state.replacements[index].size(), PAGE_EXECUTE_READWRITE,
                &state.protections[index]))
            return Fail(failure, "prediction_guard", "call_protect", index,
                        state.targets[index], 0, PAGE_EXECUTE_READWRITE,
                        GetLastError());
        state.protection_changed[index] = true;
        state.target_may_be_changed[index] = true;
        const bool wrote =
            WriteExact(process, state.targets[index],
                       state.replacements[index].data(),
                       state.replacements[index].size());
        const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
        const bool flushed =
            wrote && FlushInstructionCache(
                         process,
                         reinterpret_cast<void*>(state.targets[index]),
                         state.replacements[index].size()) != FALSE;
        const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
        DWORD ignored = 0;
        const bool restored =
            VirtualProtectEx(
                process, reinterpret_cast<void*>(state.targets[index]),
                state.replacements[index].size(), state.protections[index],
                &ignored) != FALSE;
        if (restored)
            state.protection_changed[index] = false;
        if (!wrote || !flushed || !restored)
            return Fail(failure, "prediction_guard", "call_patch", index,
                        state.targets[index],
                        static_cast<std::uint64_t>(wrote) |
                            (static_cast<std::uint64_t>(flushed) << 1) |
                            (static_cast<std::uint64_t>(restored) << 2),
                        7, !wrote ? write_error
                                  : !flushed ? flush_error
                                             : GetLastError());
    }
    state.applied = true;
    return VerifyPredictionGuard(process, state, failure);
}

bool RollbackPredictionGuard(HANDLE process,
                             PredictionGuardState& state) {
    const bool ever_published = std::any_of(
        state.target_may_be_changed.begin(),
        state.target_may_be_changed.end(), [](bool value) { return value; });
    for (std::size_t reverse = state.targets.size(); reverse != 0;
         --reverse) {
        const std::size_t index = reverse - 1;
        if (!state.target_may_be_changed[index])
            continue;
        DWORD previous = 0;
        bool protected_for_write = state.protection_changed[index];
        if (!protected_for_write)
            protected_for_write =
                VirtualProtectEx(
                    process,
                    reinterpret_cast<void*>(state.targets[index]),
                    kPredictionGuardOriginalCalls[index].size(),
                    PAGE_EXECUTE_READWRITE, &previous) != FALSE;
        const bool wrote =
            protected_for_write &&
            WriteExact(process, state.targets[index],
                       kPredictionGuardOriginalCalls[index].data(),
                       kPredictionGuardOriginalCalls[index].size());
        const bool flushed =
            wrote && FlushInstructionCache(
                         process,
                         reinterpret_cast<void*>(state.targets[index]),
                         kPredictionGuardOriginalCalls[index].size()) !=
                         FALSE;
        DWORD ignored = 0;
        const bool restored =
            protected_for_write &&
            VirtualProtectEx(
                process,
                reinterpret_cast<void*>(state.targets[index]),
                kPredictionGuardOriginalCalls[index].size(),
                state.protections[index], &ignored) != FALSE;
        std::array<std::uint8_t, 5> observed{};
        MEMORY_BASIC_INFORMATION memory{};
        const bool exact =
            ReadExact(process, state.targets[index], observed.data(),
                      observed.size()) &&
            observed == kPredictionGuardOriginalCalls[index];
        const bool protection_exact =
            VirtualQueryEx(
                process,
                reinterpret_cast<const void*>(state.targets[index]),
                &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT &&
            memory.Protect == state.protections[index];
        if (!protected_for_write || !wrote || !flushed || !restored ||
            !exact || !protection_exact)
            return false;
        state.target_may_be_changed[index] = false;
        state.protection_changed[index] = false;
    }
    state.applied = false;
    if (ever_published)
        return true;
    if (state.function_table_owned &&
        state.function_table_registered) {
        const DWORD pid = GetProcessId(process);
        const ModuleInfo ntdll = FindModule(pid, "ntdll.dll");
        const std::uint64_t delete_function_table_rva =
            ExportRva(ntdll, "RtlDeleteFunctionTable");
        std::uint64_t deletion_result = 0;
        Failure ignored{};
        if (!ntdll.base || !delete_function_table_rva ||
            !RemoteCall3(
                process, ntdll.base + delete_function_table_rva,
                state.allocation + kPredictionGuardFunctionTableOffset,
                0, 0, deletion_result, ignored,
                "prediction_guard_unwind_rollback", 0) ||
            deletion_result == 0)
            return false;
        state.function_table_registered = false;
        state.function_table_owned = false;
    }
    if (state.allocation_owned && state.allocation &&
        !VirtualFreeEx(process,
                       reinterpret_cast<void*>(state.allocation), 0,
                       MEM_RELEASE))
        return false;
    state = {};
    return true;
}

constexpr std::array<std::uint8_t, 96>
    kClientCommandListFingerprint{{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41,
        0x8B, 0x40, 0x08, 0x49, 0x8B, 0xD8, 0x4D, 0x8B,
        0x08, 0x8B, 0xFA, 0x89, 0x44, 0x24, 0x20, 0x48,
        0x8B, 0xF1, 0xE8, 0x59, 0xE1, 0xFF, 0xFF, 0x48,
        0x8B, 0x5B, 0x18, 0x48, 0x85, 0xDB, 0x74, 0x20,
        0x8B, 0x43, 0x08, 0x4C, 0x8B, 0xC3, 0x4C, 0x8B,
        0x0B, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0x89, 0x44,
        0x24, 0x20, 0xE8, 0x39, 0xE1, 0xFF, 0xFF, 0x48,
        0x8B, 0x5B, 0x18, 0x48, 0x85, 0xDB, 0x75, 0xE0,
        0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x8B, 0x74,
        0x24, 0x48, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3,
    }};

constexpr std::array<std::uint8_t, 6> kClientCommandListOriginal{{
    0x8B, 0x43, 0x08, 0x4C, 0x8B, 0xC3,
}};

constexpr std::array<std::uint8_t, 36>
    kClientCommandCalleeFingerprint{{
        0x40, 0x55, 0x41, 0x54, 0x48, 0x8D, 0xAC, 0x24,
        0xC8, 0xFC, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x38,
        0x04, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xCE, 0xBA,
        0x61, 0x00, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x85,
        0x90, 0x02, 0x00, 0x00,
    }};

bool ClientCommandCalleeFingerprintMatches(
    HANDLE process, const ModuleInfo& client, std::uint64_t address,
    const std::array<std::uint8_t,
                     kClientCommandCalleeFingerprint.size()>& bytes) {
    for (std::size_t index = 0;
         index != kClientCommandCalleeFingerprint.size(); ++index) {
        if (index >= 0x16 && index <= 0x19)
            continue;
        if (bytes[index] != kClientCommandCalleeFingerprint[index])
            return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 0x16,
                sizeof(displacement));
    const std::int64_t target =
        static_cast<std::int64_t>(address + 0x1A) + displacement;
    return target > 0 &&
           RangeInModule(static_cast<std::uint64_t>(target),
                         sizeof(std::uint64_t), client) &&
           ReadableRange(process, static_cast<std::uint64_t>(target),
                         sizeof(std::uint64_t));
}

struct ClientCommandListGuardState {
    std::uint64_t target = 0;
    std::uint64_t resume = 0;
    std::uint64_t epilogue = 0;
    std::uint64_t allocation = 0;
    std::array<std::uint8_t, 32> code{};
    std::array<std::uint8_t, 6> replacement{};
    DWORD protection = 0;
    bool protection_changed = false;
    bool target_may_be_changed = false;
    bool installed = false;
    bool allocation_owned = false;
    bool changed = false;
};

bool BuildClientCommandListGuard(ClientCommandListGuardState& state,
                                 Failure& failure) {
    state.code = {{
        0x9C, 0x50, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x48,
        0x39, 0xC3, 0x75, 0x07, 0x58, 0x9D, 0xE9, 0x00,
        0x00, 0x00, 0x00, 0x58, 0x9D, 0x8B, 0x43, 0x08,
        0x4C, 0x8B, 0xC3, 0xE9, 0x00, 0x00, 0x00, 0x00,
    }};
    auto write_relative = [&](std::size_t offset,
                              std::uint64_t destination) {
        const std::int64_t relative =
            static_cast<std::int64_t>(destination) -
            static_cast<std::int64_t>(state.allocation + offset + 5);
        if (relative < INT32_MIN || relative > INT32_MAX)
            return false;
        const std::int32_t displacement =
            static_cast<std::int32_t>(relative);
        std::memcpy(state.code.data() + offset + 1, &displacement,
                    sizeof(displacement));
        return true;
    };
    if (!write_relative(14, state.epilogue) ||
        !write_relative(27, state.resume))
        return Fail(failure, "client_command_list_guard",
                    "stub_relative_range", 0, state.allocation,
                    state.resume, state.epilogue);
    const std::int64_t relative =
        static_cast<std::int64_t>(state.allocation) -
        static_cast<std::int64_t>(state.target + 5);
    if (relative < INT32_MIN || relative > INT32_MAX)
        return Fail(failure, "client_command_list_guard",
                    "relative_range", 0, state.target,
                    state.allocation, INT32_MAX);
    state.replacement[0] = 0xE9;
    const std::int32_t displacement =
        static_cast<std::int32_t>(relative);
    std::memcpy(state.replacement.data() + 1, &displacement,
                sizeof(displacement));
    state.replacement[5] = 0x90;
    return true;
}

bool ClientCommandListFingerprintMatches(const std::uint8_t* bytes,
                                         std::size_t available) {
    if (!bytes || available < kClientCommandListFingerprint.size())
        return false;
    for (std::size_t index = 0;
         index != kClientCommandListFingerprint.size(); ++index) {
        const bool relative =
            (index >= 0x23 && index <= 0x26) ||
            (index >= 0x43 && index <= 0x46) ||
            (index >= 0x30 && index <= 0x35);
        if (!relative && bytes[index] != kClientCommandListFingerprint[index])
            return false;
    }
    std::int32_t first_displacement = 0;
    std::int32_t second_displacement = 0;
    std::memcpy(&first_displacement, bytes + 0x23,
                sizeof(first_displacement));
    std::memcpy(&second_displacement, bytes + 0x43,
                sizeof(second_displacement));
    return static_cast<std::int64_t>(first_displacement) ==
           static_cast<std::int64_t>(second_displacement) + 0x20;
}

bool PrepareClientCommandListGuard(
    HANDLE process, const ModuleInfo& client,
    ClientCommandListGuardState& state, Failure& failure) {
    std::vector<ExecutableSection> sections;
    if (!LoadExecutableSections(process, client, sections, failure, 0))
        return false;
    std::uint32_t function_rva = 0;
    std::size_t matches = 0;
    for (const auto& section : sections) {
        if (section.bytes.size() < kClientCommandListFingerprint.size())
            continue;
        for (std::size_t offset = 0;
             offset <= section.bytes.size() -
                           kClientCommandListFingerprint.size();
             ++offset) {
            const auto* bytes = section.bytes.data() + offset;
            if (!ClientCommandListFingerprintMatches(
                    bytes, section.bytes.size() - offset))
                continue;
            std::int32_t first_displacement = 0;
            std::int32_t second_displacement = 0;
            std::memcpy(&first_displacement, bytes + 0x23,
                        sizeof(first_displacement));
            std::memcpy(&second_displacement, bytes + 0x43,
                        sizeof(second_displacement));
            const std::uint64_t entry =
                client.base + section.rva + offset;
            const std::int64_t first_target =
                static_cast<std::int64_t>(entry + 0x27) +
                first_displacement;
            const std::int64_t second_target =
                static_cast<std::int64_t>(entry + 0x47) +
                second_displacement;
            if (first_target <= 0 || first_target != second_target ||
                !RangeInModule(
                    static_cast<std::uint64_t>(first_target),
                    kClientCommandCalleeFingerprint.size(), client) ||
                !ExecutableRange(process,
                                 static_cast<std::uint64_t>(first_target),
                                 kClientCommandCalleeFingerprint.size()))
                continue;
            std::array<std::uint8_t,
                       kClientCommandCalleeFingerprint.size()>
                callee{};
            if (!ReadExact(process,
                           static_cast<std::uint64_t>(first_target),
                           callee.data(), callee.size()) ||
                !ClientCommandCalleeFingerprintMatches(
                    process, client,
                    static_cast<std::uint64_t>(first_target), callee))
                continue;
            function_rva = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(section.rva) + offset);
            ++matches;
        }
    }
    if (matches != 1)
        return Fail(failure, "client_command_list_guard",
                    "fingerprint_unique", 0,
                    matches ? client.base + function_rva : client.base,
                    matches, 1);
    state.target = client.base + function_rva + 0x30;
    state.resume = client.base + function_rva + 0x36;
    state.epilogue = client.base + function_rva + 0x50;
    if (!RangeInModule(state.target, kClientCommandListOriginal.size(),
                       client) ||
        !RangeInModule(state.resume, 1, client) ||
        !RangeInModule(state.epilogue, 16, client) ||
        !ExecutableRange(process, state.target,
                         kClientCommandListOriginal.size()) ||
        !ExecutableRange(process, state.resume) ||
        !ExecutableRange(process, state.epilogue, 16))
        return false;
    constexpr std::array<std::uint8_t, 16> epilogue{{
        0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x8B, 0x74,
        0x24, 0x48, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3,
    }};
    if (!VerifyBytes(process, state.epilogue, epilogue.data(),
                     epilogue.size(), failure,
                     "client_command_list_guard", "epilogue_exact", 0))
        return false;
    std::array<std::uint8_t, kClientCommandListOriginal.size()> live{};
    if (!ReadExact(process, state.target, live.data(), live.size()))
        return Fail(failure, "client_command_list_guard", "target_read", 0,
                    state.target, 0, 0, GetLastError());
    if (live == kClientCommandListOriginal)
        return true;
    if (live[0] != 0xE9 || live[5] != 0x90)
        return Fail(failure, "client_command_list_guard",
                    "target_known_state", 0, state.target,
                    PrefixValue(live.data(), live.size()),
                    PrefixValue(kClientCommandListOriginal.data(),
                                kClientCommandListOriginal.size()));
    std::int32_t displacement = 0;
    std::memcpy(&displacement, live.data() + 1, sizeof(displacement));
    const std::int64_t destination =
        static_cast<std::int64_t>(state.target + 5) + displacement;
    if (destination <= 0)
        return Fail(failure, "client_command_list_guard",
                    "target_destination", 0, state.target,
                    static_cast<std::uint64_t>(destination), 1);
    state.allocation = static_cast<std::uint64_t>(destination);
    if (!BuildClientCommandListGuard(state, failure))
        return false;
    if (live != state.replacement)
        return Fail(failure, "client_command_list_guard",
                    "target_replacement_exact", 0, state.target,
                    PrefixValue(live.data(), live.size()),
                    PrefixValue(state.replacement.data(),
                                state.replacement.size()));
    std::array<std::uint8_t, 32> code{};
    MEMORY_BASIC_INFORMATION stub_memory{};
    MEMORY_BASIC_INFORMATION target_memory{};
    if (!ReadExact(process, state.allocation, code.data(), code.size()) ||
        code != state.code ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &stub_memory, sizeof(stub_memory)) !=
            sizeof(stub_memory) ||
        stub_memory.State != MEM_COMMIT ||
        stub_memory.Type != MEM_PRIVATE ||
        stub_memory.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<std::uint64_t>(stub_memory.AllocationBase) !=
            state.allocation ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.target),
                       &target_memory, sizeof(target_memory)) !=
            sizeof(target_memory) ||
        target_memory.State != MEM_COMMIT ||
        (target_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return Fail(failure, "client_command_list_guard",
                    "installed_state_exact", 0, state.target,
                    state.allocation, 1, GetLastError());
    state.protection = target_memory.Protect;
    state.installed = true;
    return true;
}

void* AllocateGuardInRange(HANDLE process, std::uint64_t lower,
                           std::uint64_t upper, std::size_t size) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uint64_t granularity = system.dwAllocationGranularity;
    const std::uint64_t minimum = reinterpret_cast<std::uint64_t>(
        system.lpMinimumApplicationAddress);
    const std::uint64_t maximum = reinterpret_cast<std::uint64_t>(
        system.lpMaximumApplicationAddress);
    if (!process || !granularity || !size || size - 1 > maximum)
        return nullptr;
    lower = std::max(lower, minimum);
    upper = std::min(upper, maximum - (size - 1));
    if (lower > upper)
        return nullptr;
    auto align_up = [granularity](std::uint64_t value,
                                  std::uint64_t& aligned) {
        const std::uint64_t remainder = value % granularity;
        const std::uint64_t increment =
            remainder ? granularity - remainder : 0;
        if (increment > UINT64_MAX - value)
            return false;
        aligned = value + increment;
        return true;
    };
    std::uint64_t cursor = 0;
    if (!align_up(lower, cursor))
        return nullptr;
    while (cursor <= upper) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &memory, sizeof(memory)) != sizeof(memory))
            break;
        const std::uint64_t region =
            reinterpret_cast<std::uint64_t>(memory.BaseAddress);
        if (memory.RegionSize > UINT64_MAX - region)
            break;
        const std::uint64_t region_end = region + memory.RegionSize;
        if (memory.State == MEM_FREE) {
            std::uint64_t candidate = 0;
            const bool candidate_aligned =
                align_up(std::max(cursor, region), candidate);
            if (candidate_aligned && candidate < region_end &&
                candidate <= upper && size <= region_end - candidate) {
                void* allocation = VirtualAllocEx(
                    process, reinterpret_cast<void*>(candidate), size,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (allocation) {
                    if (reinterpret_cast<std::uint64_t>(allocation) ==
                        candidate)
                        return allocation;
                    VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
                }
            }
        }
        if (region_end <= cursor)
            break;
        if (!align_up(region_end, cursor))
            break;
    }
    return nullptr;
}

void* AllocateGuardNear(HANDLE process, std::uint64_t target,
                        std::size_t size) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uint64_t maximum = reinterpret_cast<std::uint64_t>(
        system.lpMaximumApplicationAddress);
    if (!target || target > maximum - 5)
        return nullptr;
    const std::uint64_t source_after = target + 5;
    const std::uint64_t reach = 0x7FFF0000ull;
    const std::uint64_t lower =
        source_after > reach ? source_after - reach : 0;
    const std::uint64_t upper =
        source_after <= maximum - reach ? source_after + reach : maximum;
    return AllocateGuardInRange(process, lower, upper, size);
}

bool VerifyClientCommandListGuard(
    HANDLE process, const ClientCommandListGuardState& state,
    Failure& failure) {
    if ((!state.changed && !state.installed) || !state.target ||
        !state.resume ||
        !state.epilogue || !state.allocation)
        return Fail(failure, "client_command_list_guard", "state_complete",
                    0, state.target, state.allocation, 1);
    std::array<std::uint8_t, 6> replacement{};
    std::array<std::uint8_t, 32> code{};
    if (!ReadExact(process, state.target, replacement.data(),
                   replacement.size()) ||
        replacement != state.replacement ||
        !ReadExact(process, state.allocation, code.data(), code.size()) ||
        code != state.code)
        return Fail(failure, "client_command_list_guard", "bytes_exact", 0,
                    state.target, PrefixValue(replacement.data(),
                                              replacement.size()),
                    PrefixValue(state.replacement.data(),
                                state.replacement.size()),
                    GetLastError());
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
        (memory.Protect & 0xFFu) != PAGE_EXECUTE_READ ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        reinterpret_cast<std::uint64_t>(memory.AllocationBase) !=
            state.allocation)
        return Fail(failure, "client_command_list_guard", "stub_rx_exact",
                    0, state.allocation, memory.Protect,
                    PAGE_EXECUTE_READ, GetLastError());
    MEMORY_BASIC_INFORMATION target_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.target),
                       &target_memory, sizeof(target_memory)) !=
            sizeof(target_memory) ||
        target_memory.State != MEM_COMMIT ||
        target_memory.Protect != state.protection ||
        (target_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return Fail(failure, "client_command_list_guard",
                    "target_protection_exact", 0, state.target,
                    target_memory.Protect, state.protection,
                    GetLastError());
    std::int32_t target_displacement = 0;
    std::int32_t epilogue_displacement = 0;
    std::int32_t resume_displacement = 0;
    std::memcpy(&target_displacement, state.replacement.data() + 1,
                sizeof(target_displacement));
    std::memcpy(&epilogue_displacement, state.code.data() + 15,
                sizeof(epilogue_displacement));
    std::memcpy(&resume_displacement, state.code.data() + 28,
                sizeof(resume_displacement));
    const std::int64_t target_destination =
        static_cast<std::int64_t>(state.target + 5) +
        target_displacement;
    const std::int64_t epilogue_destination =
        static_cast<std::int64_t>(state.allocation + 19) +
        epilogue_displacement;
    const std::int64_t resume_destination =
        static_cast<std::int64_t>(state.allocation + 32) +
        resume_displacement;
    if (target_destination != static_cast<std::int64_t>(state.allocation))
        return Fail(failure, "client_command_list_guard",
                    "target_jump_destination", 0, state.target,
                    static_cast<std::uint64_t>(target_destination),
                    state.allocation);
    if (epilogue_destination != static_cast<std::int64_t>(state.epilogue))
        return Fail(failure, "client_command_list_guard",
                    "epilogue_jump_destination", 0,
                    state.allocation + 14,
                    static_cast<std::uint64_t>(epilogue_destination),
                    state.epilogue);
    if (resume_destination != static_cast<std::int64_t>(state.resume))
        return Fail(failure, "client_command_list_guard",
                    "resume_jump_destination", 0,
                    state.allocation + 27,
                    static_cast<std::uint64_t>(resume_destination),
                    state.resume);
    return ExecutableRange(process, state.allocation, state.code.size())
               ? true
               : Fail(failure, "client_command_list_guard",
                       "stub_executable", 0, state.allocation, 0, 1,
                       GetLastError());
}

bool ApplyClientCommandListGuard(HANDLE process,
                                 ClientCommandListGuardState& state,
                                 Failure& failure) {
    if (!state.target || !state.resume || !state.epilogue)
        return Fail(failure, "client_command_list_guard", "prepared", 0,
                    state.target, 0, 1);
    if (state.installed)
        return VerifyClientCommandListGuard(process, state, failure);
    void* allocation = AllocateGuardNear(process, state.target, 0x1000);
    if (!allocation)
        return Fail(failure, "client_command_list_guard", "allocate_near",
                    0, state.target, 0, 1, GetLastError());
    state.allocation = reinterpret_cast<std::uint64_t>(allocation);
    state.allocation_owned = true;
    if (!BuildClientCommandListGuard(state, failure))
        return false;
    if (!WriteExact(process, state.allocation, state.code.data(),
                    state.code.size()))
        return Fail(failure, "client_command_list_guard", "stub_write", 0,
                    state.allocation, 0, state.code.size(), GetLastError());
    DWORD allocation_protection = 0;
    const bool protected_rx = VirtualProtectEx(
        process, allocation, 0x1000, PAGE_EXECUTE_READ,
        &allocation_protection) != FALSE;
    const DWORD protect_error = protected_rx ? ERROR_SUCCESS : GetLastError();
    const bool stub_flushed =
        FlushInstructionCache(process, allocation, state.code.size()) != FALSE;
    const DWORD stub_flush_error =
        stub_flushed ? ERROR_SUCCESS : GetLastError();
    if (!protected_rx)
        return Fail(failure, "client_command_list_guard", "stub_protect",
                    0, state.allocation, allocation_protection,
                    PAGE_EXECUTE_READ, protect_error);
    if (!stub_flushed)
        return Fail(failure, "client_command_list_guard", "stub_flush", 0,
                    state.allocation, 0, 1, stub_flush_error);
    if (!VerifyBytes(process, state.target,
                     kClientCommandListOriginal.data(),
                     kClientCommandListOriginal.size(), failure,
                     "client_command_list_guard", "target_recheck", 0))
        return false;
    if (!VirtualProtectEx(process,
                          reinterpret_cast<void*>(state.target),
                          state.replacement.size(), PAGE_EXECUTE_READWRITE,
                          &state.protection))
        return Fail(failure, "client_command_list_guard", "target_protect",
                    0, state.target, 0, PAGE_EXECUTE_READWRITE,
                    GetLastError());
    state.protection_changed = true;
    state.target_may_be_changed = true;
    const bool wrote = WriteExact(process, state.target,
                                  state.replacement.data(),
                                  state.replacement.size());
    const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
    const bool target_flushed =
        FlushInstructionCache(process,
                              reinterpret_cast<void*>(state.target),
                              state.replacement.size()) != FALSE;
    const DWORD target_flush_error =
        target_flushed ? ERROR_SUCCESS : GetLastError();
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(state.target),
                         state.replacement.size(), state.protection,
                         &ignored) != FALSE;
    const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    if (restored)
        state.protection_changed = false;
    if (!wrote)
        return Fail(failure, "client_command_list_guard", "target_write", 0,
                    state.target, 0,
                    PrefixValue(state.replacement.data(),
                                state.replacement.size()),
                    write_error);
    if (!target_flushed)
        return Fail(failure, "client_command_list_guard", "target_flush", 0,
                    state.target, 0, 1, target_flush_error);
    if (!restored)
        return Fail(failure, "client_command_list_guard", "target_restore",
                    0, state.target, 0, state.protection, restore_error);
    state.changed = true;
    return VerifyClientCommandListGuard(process, state, failure);
}

bool RollbackClientCommandListGuard(
    HANDLE process, ClientCommandListGuardState& state) {
    const bool ever_published = state.target_may_be_changed;
    if (state.target_may_be_changed) {
        DWORD previous = 0;
        bool protected_for_write = state.protection_changed;
        if (!protected_for_write)
            protected_for_write =
                VirtualProtectEx(process,
                                 reinterpret_cast<void*>(state.target),
                                 kClientCommandListOriginal.size(),
                                 PAGE_EXECUTE_READWRITE, &previous) != FALSE;
        const bool wrote =
            protected_for_write &&
            WriteExact(process, state.target,
                       kClientCommandListOriginal.data(),
                       kClientCommandListOriginal.size());
        const bool flushed =
            wrote && FlushInstructionCache(
                         process, reinterpret_cast<void*>(state.target),
                         kClientCommandListOriginal.size()) != FALSE;
        DWORD ignored = 0;
        const bool restored =
            protected_for_write &&
            VirtualProtectEx(process,
                             reinterpret_cast<void*>(state.target),
                             kClientCommandListOriginal.size(),
                             state.protection, &ignored) != FALSE;
        std::array<std::uint8_t, 6> observed{};
        MEMORY_BASIC_INFORMATION target_memory{};
        const bool exact =
            ReadExact(process, state.target, observed.data(),
                       observed.size()) &&
            observed == kClientCommandListOriginal;
        const bool protection_exact =
            VirtualQueryEx(process,
                           reinterpret_cast<const void*>(state.target),
                           &target_memory, sizeof(target_memory)) ==
                sizeof(target_memory) &&
            target_memory.State == MEM_COMMIT &&
            target_memory.Protect == state.protection &&
            (target_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (!protected_for_write || !wrote || !flushed || !restored ||
            !exact || !protection_exact)
            return false;
        state.target_may_be_changed = false;
        state.protection_changed = false;
        state.changed = false;
    }
    if (ever_published)
        return true;
    if (state.allocation_owned && state.allocation &&
        !VirtualFreeEx(process,
                       reinterpret_cast<void*>(state.allocation), 0,
                       MEM_RELEASE))
        return false;
    state = {};
    return true;
}

constexpr std::array<std::uint8_t, 5> kBacktrackPreMoveOriginal{{
    0xE8, 0x87, 0x70, 0xD1, 0xFF,
}};
constexpr std::array<std::uint8_t, 5> kBacktrackPreMoveReplacement{{
    0x90, 0x90, 0x90, 0x90, 0x90,
}};
constexpr std::array<std::uint8_t, 23> kBacktrackPostMoveOriginal{{
    0x48, 0x8B, 0x05, 0x33, 0xBC, 0x4E, 0x00,
    0x4C, 0x8B, 0xC3, 0x41, 0x0F, 0x28, 0xC9, 0x49, 0x8B,
    0xCE, 0xFF, 0xD0, 0x44, 0x0F, 0xB6, 0xF0,
}};

struct BacktrackPostMoveState {
    std::uint64_t pre_target = 0;
    std::uint64_t post_target = 0;
    std::uint64_t resume = 0;
    std::uint64_t allocation = 0;
    std::array<std::uint8_t, kBacktrackPostMoveOriginal.size()>
        post_replacement{};
    std::array<std::uint8_t, 59> code{};
    DWORD pre_protection = 0;
    DWORD post_protection = 0;
    bool pre_protection_changed = false;
    bool post_protection_changed = false;
    bool pre_target_may_be_changed = false;
    bool post_target_may_be_changed = false;
    bool pre_installed = false;
    bool post_installed = false;
    bool allocation_owned = false;
    bool pre_changed = false;
    bool post_changed = false;
};

bool BuildBacktrackPostMove(std::uint64_t image_base,
                            BacktrackPostMoveState& state,
                            Failure& failure) {
    state.code = {{
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x48, 0x8B, 0x00, 0x4C, 0x8B, 0xC3,
        0x41, 0x0F, 0x28, 0xC9, 0x49, 0x8B, 0xCE, 0xFF,
        0xD0, 0x44, 0x0F, 0xB6, 0xF0, 0x48, 0x8B, 0xD3,
        0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0xE9, 0x00,
        0x00, 0x00, 0x00,
    }};
    const std::uint64_t saved_original_slot = image_base + 0x7EA918;
    const std::uint64_t records_owner = image_base + 0x7D4AC0;
    const std::uint64_t consumer = image_base + 0x138B0;
    std::memcpy(state.code.data() + 2, &saved_original_slot,
                sizeof(saved_original_slot));
    std::memcpy(state.code.data() + 34, &records_owner,
                sizeof(records_owner));
    std::memcpy(state.code.data() + 44, &consumer, sizeof(consumer));
    const std::int64_t resume_relative =
        static_cast<std::int64_t>(state.resume) -
        static_cast<std::int64_t>(state.allocation + state.code.size());
    if (resume_relative < INT32_MIN || resume_relative > INT32_MAX)
        return Fail(failure, "backtrack_post_move", "resume_reach", 0,
                    state.allocation, state.resume, INT32_MAX);
    const auto resume_displacement =
        static_cast<std::int32_t>(resume_relative);
    std::memcpy(state.code.data() + 55, &resume_displacement,
                sizeof(resume_displacement));
    const std::int64_t post_relative =
        static_cast<std::int64_t>(state.allocation) -
        static_cast<std::int64_t>(state.post_target + 5);
    if (post_relative < INT32_MIN || post_relative > INT32_MAX)
        return Fail(failure, "backtrack_post_move", "post_reach", 0,
                    state.post_target, state.allocation, INT32_MAX);
    state.post_replacement[0] = 0xE9;
    const auto post_displacement =
        static_cast<std::int32_t>(post_relative);
    std::memcpy(state.post_replacement.data() + 1, &post_displacement,
                sizeof(post_displacement));
    std::fill(state.post_replacement.begin() + 5,
              state.post_replacement.end(), 0x90);
    return true;
}

bool PrepareBacktrackPostMove(HANDLE process, std::uint64_t image_base,
                              BacktrackPostMoveState& state,
                              Failure& failure) {
    state.pre_target = image_base + 0x2FC824;
    state.post_target = image_base + 0x2FECDE;
    state.resume = state.post_target + kBacktrackPostMoveOriginal.size();
    if (!ExecutableRange(process, state.pre_target,
                         kBacktrackPreMoveOriginal.size()) ||
        !ExecutableRange(process, state.post_target,
                         kBacktrackPostMoveOriginal.size()) ||
        !ExecutableRange(process, state.resume))
        return Fail(failure, "backtrack_post_move", "range_executable", 0,
                    state.post_target, 0, state.resume, GetLastError());
    std::array<std::uint8_t, kBacktrackPreMoveOriginal.size()> pre{};
    std::array<std::uint8_t, kBacktrackPostMoveOriginal.size()> post{};
    if (!ReadExact(process, state.pre_target, pre.data(), pre.size()) ||
        !ReadExact(process, state.post_target, post.data(), post.size()))
        return Fail(failure, "backtrack_post_move", "target_read", 0,
                    state.post_target, 0, 0, GetLastError());
    if (pre == kBacktrackPreMoveReplacement) {
        state.pre_installed = true;
    } else if (pre != kBacktrackPreMoveOriginal) {
        return Fail(failure, "backtrack_post_move", "pre_known_state", 0,
                    state.pre_target, PrefixValue(pre.data(), pre.size()),
                    PrefixValue(kBacktrackPreMoveOriginal.data(),
                                kBacktrackPreMoveOriginal.size()));
    }
    if (post == kBacktrackPostMoveOriginal) {
        if (state.pre_installed) {
            MEMORY_BASIC_INFORMATION pre_memory{};
            if (VirtualQueryEx(
                    process,
                    reinterpret_cast<const void*>(state.pre_target),
                    &pre_memory, sizeof(pre_memory)) !=
                    sizeof(pre_memory) ||
                pre_memory.State != MEM_COMMIT ||
                (pre_memory.Protect &
                 (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return Fail(failure, "backtrack_post_move",
                            "pre_protection", 0, state.pre_target,
                            pre_memory.Protect, 1, GetLastError());
            state.pre_protection = pre_memory.Protect;
        }
        return true;
    }
    if (post[0] != 0xE9 ||
        !std::all_of(post.begin() + 5, post.end(),
                     [](std::uint8_t byte) { return byte == 0x90; }))
        return Fail(failure, "backtrack_post_move", "post_known_state", 0,
                    state.post_target,
                    PrefixValue(post.data(), post.size()),
                    PrefixValue(kBacktrackPostMoveOriginal.data(),
                                kBacktrackPostMoveOriginal.size()));
    std::int32_t displacement = 0;
    std::memcpy(&displacement, post.data() + 1, sizeof(displacement));
    const std::int64_t destination =
        static_cast<std::int64_t>(state.post_target + 5) + displacement;
    if (destination <= 0)
        return Fail(failure, "backtrack_post_move",
                    "post_destination", 0, state.post_target,
                    static_cast<std::uint64_t>(destination), 1);
    state.allocation = static_cast<std::uint64_t>(destination);
    if (!BuildBacktrackPostMove(image_base, state, failure))
        return false;
    if (post != state.post_replacement)
        return Fail(failure, "backtrack_post_move",
                    "post_replacement_exact", 0, state.post_target,
                    PrefixValue(post.data(), post.size()),
                    PrefixValue(state.post_replacement.data(),
                                state.post_replacement.size()));
    std::array<std::uint8_t, 59> code{};
    MEMORY_BASIC_INFORMATION stub_memory{};
    MEMORY_BASIC_INFORMATION pre_memory{};
    MEMORY_BASIC_INFORMATION post_memory{};
    if (!ReadExact(process, state.allocation, code.data(), code.size()) ||
        code != state.code ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &stub_memory, sizeof(stub_memory)) !=
            sizeof(stub_memory) ||
        stub_memory.State != MEM_COMMIT ||
        stub_memory.Type != MEM_PRIVATE ||
        stub_memory.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<std::uint64_t>(stub_memory.AllocationBase) !=
            state.allocation ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.pre_target),
                       &pre_memory, sizeof(pre_memory)) !=
            sizeof(pre_memory) ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.post_target),
                       &post_memory, sizeof(post_memory)) !=
            sizeof(post_memory) ||
        pre_memory.State != MEM_COMMIT ||
        post_memory.State != MEM_COMMIT ||
        (pre_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (post_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return Fail(failure, "backtrack_post_move",
                    "installed_state_exact", 0, state.post_target,
                    state.allocation, 1, GetLastError());
    state.pre_protection = pre_memory.Protect;
    state.post_protection = post_memory.Protect;
    state.post_installed = true;
    return true;
}

bool VerifyBacktrackPostMove(HANDLE process,
                             const BacktrackPostMoveState& state,
                             Failure& failure) {
    std::array<std::uint8_t, kBacktrackPreMoveReplacement.size()> pre{};
    std::array<std::uint8_t, kBacktrackPostMoveOriginal.size()> post{};
    std::array<std::uint8_t, 59> code{};
    if ((!state.pre_changed && !state.pre_installed) ||
        (!state.post_changed && !state.post_installed) ||
        !state.allocation ||
        !ReadExact(process, state.pre_target, pre.data(), pre.size()) ||
        !ReadExact(process, state.post_target, post.data(), post.size()) ||
        !ReadExact(process, state.allocation, code.data(), code.size()) ||
        pre != kBacktrackPreMoveReplacement ||
        post != state.post_replacement || code != state.code)
        return Fail(failure, "backtrack_post_move", "bytes_exact", 0,
                    state.post_target,
                    PrefixValue(post.data(), post.size()),
                    PrefixValue(state.post_replacement.data(),
                                state.post_replacement.size()),
                    GetLastError());
    MEMORY_BASIC_INFORMATION stub_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.allocation),
                       &stub_memory, sizeof(stub_memory)) !=
            sizeof(stub_memory) ||
        stub_memory.State != MEM_COMMIT ||
        stub_memory.Type != MEM_PRIVATE ||
        stub_memory.Protect != PAGE_EXECUTE_READ ||
        reinterpret_cast<std::uint64_t>(stub_memory.AllocationBase) !=
            state.allocation)
        return Fail(failure, "backtrack_post_move", "stub_rx_exact", 0,
                    state.allocation, stub_memory.Protect,
                    PAGE_EXECUTE_READ, GetLastError());
    MEMORY_BASIC_INFORMATION pre_memory{};
    MEMORY_BASIC_INFORMATION post_memory{};
    if (VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.pre_target),
                       &pre_memory, sizeof(pre_memory)) !=
            sizeof(pre_memory) ||
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(state.post_target),
                       &post_memory, sizeof(post_memory)) !=
            sizeof(post_memory) ||
        pre_memory.State != MEM_COMMIT || post_memory.State != MEM_COMMIT ||
        pre_memory.Protect != state.pre_protection ||
        post_memory.Protect != state.post_protection ||
        (pre_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (post_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return Fail(failure, "backtrack_post_move",
                    "target_protection_exact", 0, state.post_target,
                    post_memory.Protect, state.post_protection,
                    GetLastError());
    std::int32_t post_displacement = 0;
    std::int32_t resume_displacement = 0;
    std::memcpy(&post_displacement, state.post_replacement.data() + 1,
                sizeof(post_displacement));
    std::memcpy(&resume_displacement, state.code.data() + 55,
                sizeof(resume_displacement));
    const std::int64_t post_destination =
        static_cast<std::int64_t>(state.post_target + 5) +
        post_displacement;
    const std::int64_t resume_destination =
        static_cast<std::int64_t>(state.allocation + state.code.size()) +
        resume_displacement;
    const bool post_padding_exact =
        std::all_of(post.begin() + 5, post.end(),
                    [](std::uint8_t byte) { return byte == 0x90; });
    if (!post_padding_exact ||
        post_destination != static_cast<std::int64_t>(state.allocation) ||
        resume_destination != static_cast<std::int64_t>(state.resume))
        return Fail(failure, "backtrack_post_move", "flow_exact", 0,
                    state.post_target,
                    static_cast<std::uint64_t>(post_destination),
                    state.allocation);
    return true;
}

bool ApplyBacktrackPostMove(HANDLE process, std::uint64_t image_base,
                            BacktrackPostMoveState& state,
                            Failure& failure) {
    if (!state.pre_target || !state.post_target || !state.resume)
        return Fail(failure, "backtrack_post_move", "prepared", 0,
                    state.post_target, 0, 1);
    if (!state.allocation) {
        void* allocation =
            AllocateGuardNear(process, state.post_target, 0x1000);
        if (!allocation)
            return Fail(failure, "backtrack_post_move", "allocate_near", 0,
                        state.post_target, 0, 0x1000, GetLastError());
        state.allocation = reinterpret_cast<std::uint64_t>(allocation);
        state.allocation_owned = true;
    }
    if (!BuildBacktrackPostMove(image_base, state, failure))
        return false;
    if (state.allocation_owned) {
        if (!WriteExact(process, state.allocation, state.code.data(),
                        state.code.size()))
            return Fail(failure, "backtrack_post_move", "stub_write", 0,
                        state.allocation, 0, state.code.size(),
                        GetLastError());
        DWORD allocation_protection = 0;
        if (!VirtualProtectEx(
                process, reinterpret_cast<void*>(state.allocation), 0x1000,
                PAGE_EXECUTE_READ, &allocation_protection) ||
            !FlushInstructionCache(
                process, reinterpret_cast<void*>(state.allocation),
                state.code.size()))
            return Fail(failure, "backtrack_post_move", "stub_rx", 0,
                        state.allocation, allocation_protection,
                        PAGE_EXECUTE_READ, GetLastError());
    }
    auto patch = [&](bool installed, std::uint64_t target,
                     const std::uint8_t* original,
                     const std::uint8_t* replacement, std::size_t size,
                     DWORD& protection, bool& protection_changed,
                     bool& target_may_be_changed, bool& changed,
                     const char* stage) {
        if (installed)
            return true;
        if (!VerifyBytes(process, target, original, size, failure,
                         "backtrack_post_move", stage, 0))
            return false;
        if (!VirtualProtectEx(process, reinterpret_cast<void*>(target), size,
                              PAGE_EXECUTE_READWRITE, &protection))
            return Fail(failure, "backtrack_post_move", "target_protect", 0,
                        target, 0, PAGE_EXECUTE_READWRITE, GetLastError());
        protection_changed = true;
        target_may_be_changed = true;
        const bool wrote = WriteExact(process, target, replacement, size);
        const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
        const bool flushed =
            wrote && FlushInstructionCache(
                         process, reinterpret_cast<void*>(target), size);
        const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
        DWORD ignored = 0;
        const bool restored =
            VirtualProtectEx(process, reinterpret_cast<void*>(target), size,
                             protection, &ignored) != FALSE;
        if (restored)
            protection_changed = false;
        if (!wrote || !flushed || !restored)
            return Fail(failure, "backtrack_post_move", "target_patch", 0,
                        target,
                        static_cast<std::uint64_t>(wrote) |
                            (static_cast<std::uint64_t>(flushed) << 1) |
                            (static_cast<std::uint64_t>(restored) << 2),
                        7, !wrote ? write_error
                                  : !flushed ? flush_error : GetLastError());
        changed = true;
        return true;
    };
    if (!patch(state.post_installed, state.post_target,
               kBacktrackPostMoveOriginal.data(),
               state.post_replacement.data(),
               state.post_replacement.size(), state.post_protection,
               state.post_protection_changed,
               state.post_target_may_be_changed, state.post_changed,
               "post_recheck") ||
        !patch(state.pre_installed, state.pre_target,
               kBacktrackPreMoveOriginal.data(),
               kBacktrackPreMoveReplacement.data(),
               kBacktrackPreMoveReplacement.size(), state.pre_protection,
               state.pre_protection_changed,
               state.pre_target_may_be_changed, state.pre_changed,
               "pre_recheck"))
        return false;
    return VerifyBacktrackPostMove(process, state, failure);
}

bool RollbackBacktrackPostMove(HANDLE process,
                               BacktrackPostMoveState& state) {
    const bool ever_published =
        state.pre_target_may_be_changed ||
        state.post_target_may_be_changed;
    auto restore = [&](std::uint64_t target, const std::uint8_t* original,
                       std::size_t size, DWORD protection,
                       bool& protection_changed,
                       bool& target_may_be_changed, bool& changed) {
        if (!target_may_be_changed)
            return true;
        DWORD previous = 0;
        bool protected_for_write = protection_changed;
        if (!protected_for_write)
            protected_for_write =
                VirtualProtectEx(process, reinterpret_cast<void*>(target),
                                 size, PAGE_EXECUTE_READWRITE,
                                 &previous) != FALSE;
        const bool wrote =
            protected_for_write &&
            WriteExact(process, target, original, size);
        const bool flushed =
            wrote && FlushInstructionCache(
                         process, reinterpret_cast<void*>(target), size);
        DWORD ignored = 0;
        const bool restored =
            protected_for_write &&
            VirtualProtectEx(process, reinterpret_cast<void*>(target), size,
                             protection, &ignored) != FALSE;
        std::vector<std::uint8_t> observed(size);
        const bool exact =
            ReadExact(process, target, observed.data(), observed.size()) &&
            std::equal(observed.begin(), observed.end(), original);
        if (!protected_for_write || !wrote || !flushed || !restored ||
            !exact)
            return false;
        protection_changed = false;
        target_may_be_changed = false;
        changed = false;
        return true;
    };
    bool ok = true;
    if (!restore(state.pre_target, kBacktrackPreMoveOriginal.data(),
                 kBacktrackPreMoveOriginal.size(), state.pre_protection,
                 state.pre_protection_changed,
                 state.pre_target_may_be_changed, state.pre_changed))
        ok = false;
    if (!restore(state.post_target, kBacktrackPostMoveOriginal.data(),
                 kBacktrackPostMoveOriginal.size(), state.post_protection,
                 state.post_protection_changed,
                 state.post_target_may_be_changed, state.post_changed))
        ok = false;
    if (ok && ever_published)
        return true;
    if (ok && state.allocation_owned && state.allocation &&
        !VirtualFreeEx(process, reinterpret_cast<void*>(state.allocation), 0,
                       MEM_RELEASE))
        ok = false;
    if (ok)
        state = {};
    return ok;
}

struct ImagePatchSpec {
    const char* stage;
    std::uint32_t rva;
    std::size_t size;
    std::array<std::uint8_t, 9> original;
    std::array<std::uint8_t, 9> replacement;
};

constexpr std::size_t kMaxImagePatchSize = 9;

struct ImagePatchState {
    std::uint64_t address = 0;
    std::size_t size = 0;
    std::array<std::uint8_t, kMaxImagePatchSize> original{};
    std::array<std::uint8_t, kMaxImagePatchSize> replacement{};
    DWORD protection = 0;
    bool changed = false;
};

constexpr std::array<ImagePatchSpec, 4> kImagePatches{{
    {"payload_createmove_cold_guard", 0x2FDEC2, 5,
     {0xE9, 0xE5, 0xE7, 0xB9, 0x00},
     {0xE9, 0xD3, 0xFF, 0xFF, 0xFF}},
    {"payload_lua_rearm_cave", 0x31E546, 9,
     {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
     {0xC6, 0x05, 0x18, 0xC0, 0x4C, 0x00, 0x01, 0xEB, 0xEC}},
    {"payload_lua_rearm_redirect", 0x31E539, 2,
     {0xEB, 0x00},
     {0xEB, 0x0B}},
    {"steam_friends_set_name", 0xB9469, 4,
     {0x48, 0x8B, 0x40, 0x08},
     {0x0F, 0x1F, 0x40, 0x00}},
}};

bool PrepareImagePatches(
    HANDLE process, std::uint64_t image_base,
    std::array<ImagePatchState, kImagePatches.size()>& states,
    Failure& failure) {
    for (std::size_t index = 0; index != kImagePatches.size(); ++index) {
        const auto& spec = kImagePatches[index];
        auto& state = states[index];
        if (!spec.size || spec.size > kMaxImagePatchSize)
            return Fail(failure, spec.stage, "size_supported", index,
                        spec.rva, spec.size, kMaxImagePatchSize);
        state.address = image_base + spec.rva;
        state.size = spec.size;
        state.original = spec.original;
        state.replacement = spec.replacement;
        if (!ExecutableRange(process, state.address, state.size))
            return Fail(failure, spec.stage, "range_executable", index,
                        state.address, 0, 1, GetLastError());
        std::array<std::uint8_t, kMaxImagePatchSize> observed{};
        if (!ReadExact(process, state.address, observed.data(), state.size))
            return Fail(failure, spec.stage, "original_read", index,
                        state.address, 0,
                        PrefixValue(state.original.data(), state.size),
                        GetLastError());
        const bool original =
            std::equal(observed.begin(), observed.begin() + state.size,
                       state.original.begin());
        const bool replacement =
            std::equal(observed.begin(), observed.begin() + state.size,
                       state.replacement.begin());
        if (!original && !replacement)
            return Fail(failure, spec.stage, "known_exact", index,
                        state.address,
                        PrefixValue(observed.data(), state.size),
                        PrefixValue(state.original.data(), state.size));
    }
    return true;
}

bool ApplyImagePatch(HANDLE process, const ImagePatchSpec& spec,
                     ImagePatchState& state, Failure& failure,
                     std::size_t index) {
    std::array<std::uint8_t, kMaxImagePatchSize> observed{};
    if (!ReadExact(process, state.address, observed.data(), state.size))
        return Fail(failure, spec.stage, "original_read", index,
                    state.address, 0,
                    PrefixValue(state.original.data(), state.size),
                    GetLastError());
    if (std::equal(observed.begin(), observed.begin() + state.size,
                   state.replacement.begin()))
        return true;
    if (!std::equal(observed.begin(), observed.begin() + state.size,
                    state.original.begin()))
        return Fail(failure, spec.stage, "original_exact", index,
                    state.address, PrefixValue(observed.data(), state.size),
                    PrefixValue(state.original.data(), state.size));
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(state.address),
                          state.size, PAGE_EXECUTE_READWRITE,
                          &state.protection))
        return Fail(failure, spec.stage, "protect_write", index,
                    state.address, 0, PAGE_EXECUTE_READWRITE,
                    GetLastError());
    state.changed = true;
    const bool wrote = WriteExact(process, state.address,
                                  state.replacement.data(), state.size);
    const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
    const bool flushed =
        FlushInstructionCache(process,
                              reinterpret_cast<void*>(state.address),
                              state.size) != FALSE;
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(state.address),
                         state.size, state.protection, &ignored) != FALSE;
    const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    if (!wrote)
        return Fail(failure, spec.stage, "write", index, state.address, 0,
                    PrefixValue(state.replacement.data(), state.size),
                    write_error);
    if (!flushed)
        return Fail(failure, spec.stage, "flush", index, state.address, 0,
                    1, flush_error);
    if (!restored)
        return Fail(failure, spec.stage, "protect_restore", index,
                    state.address, 0, state.protection, restore_error);
    observed.fill(0);
    if (!ReadExact(process, state.address, observed.data(), state.size))
        return Fail(failure, spec.stage, "replacement_read", index,
                    state.address, 0,
                    PrefixValue(state.replacement.data(), state.size),
                    GetLastError());
    if (!std::equal(observed.begin(), observed.begin() + state.size,
                    state.replacement.begin()))
        return Fail(failure, spec.stage, "replacement_exact", index,
                    state.address, PrefixValue(observed.data(), state.size),
                    PrefixValue(state.replacement.data(), state.size));
    return true;
}

bool RollbackImagePatches(
    HANDLE process,
    std::array<ImagePatchState, kImagePatches.size()>& states) {
    bool ok = true;
    for (std::size_t reverse = states.size(); reverse != 0; --reverse) {
        auto& state = states[reverse - 1];
        if (!state.changed)
            continue;
        DWORD previous = 0;
        const bool protected_for_write =
            VirtualProtectEx(process, reinterpret_cast<void*>(state.address),
                             state.size, PAGE_EXECUTE_READWRITE,
                             &previous) != FALSE;
        const bool wrote = WriteExact(process, state.address,
                                      state.original.data(), state.size);
        const bool flushed =
            FlushInstructionCache(process,
                                  reinterpret_cast<void*>(state.address),
                                  state.size) != FALSE;
        DWORD ignored = 0;
        const bool restored =
            VirtualProtectEx(process, reinterpret_cast<void*>(state.address),
                             state.size, state.protection, &ignored) != FALSE;
        std::array<std::uint8_t, kMaxImagePatchSize> observed{};
        const bool read =
            ReadExact(process, state.address, observed.data(), state.size);
        const bool exact =
            read && std::equal(observed.begin(),
                               observed.begin() + state.size,
                               state.original.begin());
        if (protected_for_write && wrote && flushed && restored && exact)
            state.changed = false;
        else
            ok = false;
    }
    return ok;
}

bool PrepareDirect(HANDLE process, std::uint64_t image_base,
                   const DirectHookSpec& spec, const ModuleInfo& module,
                   DirectState& state, Failure& failure, std::size_t index) {
    const std::uint64_t object_slot = image_base + spec.object_slot_rva;
    std::uint64_t live = 0;
    std::uint64_t saved = 0;
    if (!ReadValue(process, object_slot, state.object) || !state.object ||
        !ReadValue(process, state.object, state.vtable) || !state.vtable) {
        return Fail(failure, "direct_pre", "object_vtable", index,
                    object_slot, state.object, 1, GetLastError());
    }
    state.entry_slot =
        state.vtable + static_cast<std::uint64_t>(spec.vtable_index) * 8;
    if (!ReadValue(process, state.entry_slot, live) ||
        !ReadValue(process, image_base + spec.saved_slot_rva, saved))
        return Fail(failure, "direct_pre", "entry_saved_read", index,
                    state.entry_slot, 0, 0, GetLastError());
    const std::uint64_t expected_detour = image_base + spec.detour_rva;
    if (live == expected_detour && saved) {
        state.installed = true;
        state.saved_present = true;
        state.original = saved;
    } else if (live != expected_detour && saved == 0) {
        state.original = live;
    } else if (live != expected_detour && saved == live) {
        state.saved_present = true;
        state.original = saved;
    } else {
        return Fail(failure, "direct_pre", "entry_saved_consistent", index,
                    state.entry_slot, live, expected_detour);
    }
    if (!VerifyOriginal(process, spec, module, state.original, failure,
                        index, "direct_pre") ||
        !ExecutableRange(process, expected_detour, kDetourPrefixSize) ||
        !VerifyBytes(process, expected_detour, spec.detour_prefix,
                     kDetourPrefixSize, failure, "direct_pre",
                     "detour_prefix", index))
        return false;
    return true;
}

bool VerifyDirect(HANDLE process, std::uint64_t image_base,
                  const DirectHookSpec& spec, const ModuleInfo& module,
                  const DirectState& state, Failure& failure,
                  std::size_t index) {
    std::uint64_t live = 0;
    std::uint64_t saved = 0;
    const std::uint64_t expected_detour = image_base + spec.detour_rva;
    const std::uint64_t expected_original = state.original;
    if (!ReadValue(process, state.entry_slot, live) ||
        !ReadValue(process, image_base + spec.saved_slot_rva, saved))
        return Fail(failure, "direct_post", "entry_saved_read", index,
                    state.entry_slot, 0, 0, GetLastError());
    if (live != expected_detour || saved != expected_original)
        return Fail(failure, "direct_post", "entry_saved_exact", index,
                    state.entry_slot, live, expected_detour);
    if (!VerifyOriginal(process, spec, module, saved, failure, index,
                        "direct_post"))
        return false;
    return VerifyBytes(process, expected_detour, spec.detour_prefix,
                       kDetourPrefixSize, failure, "direct_post",
                       "detour_prefix", index);
}

bool ApplyDirect(HANDLE process, std::uint64_t image_base,
                 const DirectHookSpec& spec, DirectState& state,
                 Failure& failure, std::size_t index) {
    if (state.installed)
        return true;
    const std::uint64_t saved_slot = image_base + spec.saved_slot_rva;
    if (!state.saved_present) {
        state.saved_changed = true;
        if (!WriteProtectedQword(process, saved_slot, state.original))
            return Fail(failure, "direct_apply", "saved_write", index,
                        saved_slot, 0, state.original, GetLastError());
        state.saved_present = true;
    }
    state.entry_changed = true;
    if (!WriteProtectedQword(process, state.entry_slot,
                             image_base + spec.detour_rva)) {
        if (WriteProtectedQword(process, state.entry_slot, state.original))
            state.entry_changed = false;
        if (!state.entry_changed && state.saved_changed &&
            WriteProtectedQword(process, saved_slot, 0)) {
            state.saved_changed = false;
            state.saved_present = false;
        }
        state.changed = state.entry_changed || state.saved_changed;
        return Fail(failure, "direct_apply", "entry_write", index,
                    state.entry_slot, 0, image_base + spec.detour_rva,
                    GetLastError());
    }
    state.changed = state.entry_changed || state.saved_changed;
    return true;
}

struct ReplacementState {
    std::uint64_t slot = 0;
    std::uint64_t original = 0;
    bool installed = false;
    bool changed = false;
};

bool PrepareReplacement(HANDLE process, std::uint64_t image_base,
                        const ModuleInfo& client, ReplacementState& state,
                        Failure& failure) {
    std::uint64_t input = 0;
    if (!ReadValue(process, image_base + 0x7EA820, input) ||
        !RangeInModule(input, sizeof(std::uint64_t), client) ||
        !ReadableRange(process, input, sizeof(std::uint64_t)))
        return Fail(failure, "replacement_pre", "input_read", 0,
                    image_base + 0x7EA820, input, 1, GetLastError());
    std::uint64_t vtable = 0;
    if (!ReadValue(process, input, vtable) ||
        !RangeInModule(vtable, 9 * sizeof(std::uint64_t), client) ||
        !ReadableRange(process, vtable, 9 * sizeof(std::uint64_t)))
        return Fail(failure, "replacement_pre", "vtable_read", 0,
                    input, vtable, client.base, GetLastError());
    state.slot = vtable + 8 * sizeof(std::uint64_t);
    if (!ReadValue(process, state.slot, state.original))
        return Fail(failure, "replacement_pre", "slot_read", 0,
                    state.slot, 0, 0, GetLastError());
    const std::uint64_t detour = image_base + 0x3359D0;
    if (state.original == detour) {
        state.installed = true;
    } else if (!RangeInModule(state.original, 61, client) ||
               !ExecutableRange(process, state.original, 61)) {
        return Fail(failure, "replacement_pre", "original_executable", 0,
                    state.slot, state.original, client.base);
    }
    static constexpr std::array<std::uint8_t, 61> original_prefix{{
        0x44, 0x8B, 0xCA, 0x4C, 0x8B, 0xC1, 0xB8, 0xB7,
        0x60, 0x0B, 0xB6, 0x41, 0x8B, 0xC9, 0xF7, 0xEA,
        0x41, 0x03, 0xD1, 0xC1, 0xFA, 0x06, 0x8B, 0xC2,
        0xC1, 0xE8, 0x1F, 0x03, 0xD0, 0x6B, 0xC2, 0x5A,
        0x2B, 0xC8, 0x48, 0x63, 0xC1, 0x33, 0xC9, 0x48,
        0x69, 0xC0, 0x3C, 0x01, 0x00, 0x00, 0x49, 0x03,
        0x80, 0xD0, 0x00, 0x00, 0x00, 0x44, 0x39, 0x08,
        0x48, 0x0F, 0x45, 0xC1, 0xC3,
    }};
    if (!state.installed &&
        !VerifyBytes(process, state.original, original_prefix.data(),
                     original_prefix.size(), failure, "replacement_pre",
                     "original_prefix", 0))
        return false;
    if (!ExecutableRange(process, detour, kDetourPrefixSize))
        return Fail(failure, "replacement_pre", "detour_executable", 0,
                    detour);
    return VerifyBytes(process, detour, kDetour3359d0,
                       kDetourPrefixSize, failure, "replacement_pre",
                       "detour_prefix", 0);
}

bool ApplyReplacement(HANDLE process, std::uint64_t image_base,
                      ReplacementState& state, Failure& failure) {
    if (state.installed)
        return true;
    if (!WriteProtectedQword(process, state.slot,
                             image_base + 0x3359D0))
        return Fail(failure, "replacement_apply", "entry_write", 0,
                    state.slot, 0, image_base + 0x3359D0, GetLastError());
    state.changed = true;
    return true;
}

bool VerifyReplacement(HANDLE process, std::uint64_t image_base,
                       const ReplacementState& state, Failure& failure) {
    std::uint64_t observed = 0;
    const std::uint64_t expected = image_base + 0x3359D0;
    if (!ReadValue(process, state.slot, observed))
        return Fail(failure, "replacement_post", "entry_read", 0,
                    state.slot, 0, 0, GetLastError());
    if (observed != expected)
        return Fail(failure, "replacement_post", "entry_exact", 0,
                    state.slot, observed, expected);
    return VerifyBytes(process, expected, kDetour3359d0,
                       kDetourPrefixSize, failure, "replacement_post",
                       "detour_prefix", 0);
}

struct CloneRecord {
    std::uint64_t original_vtable;
    std::uint64_t cloned_vtable;
    std::uint64_t object;
    std::uint32_t index;
    std::uint32_t padding;
    std::uint64_t detour;
};

static_assert(sizeof(CloneRecord) == 0x28);

struct CloneState {
    std::uint64_t object = 0;
    std::uint64_t previous_vtable = 0;
    std::uint64_t cloned_vtable = 0;
    std::uint64_t owner = 0;
    std::uint64_t record = 0;
    std::uint64_t original = 0;
    std::vector<std::uint64_t> source_entries;
    bool installed = false;
    bool mutation_possible = false;
    bool cleanup_safe = false;
    bool saved_changed = false;
    bool owner_record_changed = false;
};

struct EventRecord {
    std::uint64_t object;
    std::uint64_t original;
    std::uint32_t index;
    std::uint32_t padding;
};

static_assert(sizeof(EventRecord) == 0x18);

struct EventVector {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
};

struct EventState {
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry_slot = 0;
    std::uint64_t original = 0;
    std::uint64_t registration_record = 0;
    EventVector vector_before{};
    EventVector vector_after{};
    std::vector<EventRecord> records_before;
    bool installed = false;
    bool mutation_possible = false;
    bool saved_changed = false;
    bool registration_mutation_possible = false;
    bool registration_changed = false;
    bool pending = false;
};

struct LuaBootstrapState {
    std::uint64_t realm = 0;
    std::uint64_t lua_state = 0;
    std::uint64_t menu_realm = 0;
    std::uint64_t menu_lua_state = 0;
    std::uint64_t shared_vtable = 0;
    std::uint64_t get_lua = 0;
    std::uint64_t menu_vtable = 0;
    std::array<std::uint64_t, 2> realm_vtables{};
    std::size_t realm_vtable_count = 0;
    std::uint64_t vtable = 0;
    std::uint64_t entry_slot = 0;
    std::uint64_t original = 0;
    std::uint64_t previous_realm_slot = 0;
    std::uint64_t previous_saved = 0;
    bool installed = false;
    bool deferred = false;
    bool realm_changed = false;
    bool saved_changed = false;
    bool entry_changed = false;
    bool registration_seeded = false;
    bool menu_registration_seeded = false;
    std::uint64_t linker_replay_allocation = 0;
    std::uint64_t linker_replay_context = 0;
    std::uint64_t linker_replay_thunk = 0;
    std::uint64_t linker_replay_previous_saved = 0;
    std::array<std::uint64_t, 2> linker_replay_entry_slots{};
    std::array<std::uint64_t, 2> linker_replay_entry_previous{};
    std::size_t linker_replay_entry_count = 0;
    bool linker_replay_published = false;
    bool linker_replay_saved_changed = false;
};

struct LuaLinkerReplayContext {
    std::uint32_t guard = 0;
    std::uint32_t status = 0;
    std::uint64_t realm_slot = 0;
    std::uint64_t original = 0;
    std::uint64_t gettop = 0;
    std::uint64_t settop = 0;
    std::uint64_t getfield = 0;
    std::uint64_t setfield = 0;
    std::uint64_t topointer = 0;
    std::uint64_t build_environment = 0;
    std::uint64_t is_type = 0;
    std::uint64_t pop = 0;
    std::uint64_t registrar = 0;
    std::uint64_t registrar_context = 0;
    std::uint64_t ownership_global = 0;
    std::uint64_t ownership_lookup = 0;
    std::uint64_t ownership_insert = 0;
    std::uint64_t saved_slot = 0;
    std::array<char, 8> linker_name{{'l', 'i', 'n', 'k', 'e', 'r', 0, 0}};
    std::uint64_t observed_lua_state = 0;
    std::uint64_t observed_table = 0;
    std::int32_t original_top = 0;
    std::uint32_t reserved = 0;
};

static_assert(offsetof(LuaLinkerReplayContext, realm_slot) == 0x08);
static_assert(offsetof(LuaLinkerReplayContext, original) == 0x10);
static_assert(offsetof(LuaLinkerReplayContext, linker_name) == 0x88);
static_assert(offsetof(LuaLinkerReplayContext, observed_lua_state) == 0x90);
static_assert(offsetof(LuaLinkerReplayContext, original_top) == 0xA0);
static_assert(sizeof(LuaLinkerReplayContext) == 0xA8);

constexpr std::array<std::uint8_t, 609> kLuaLinkerReplayThunk{{
        0x53, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4C, 0x24,
        0x20, 0x48, 0x89, 0x54, 0x24, 0x28, 0x4C, 0x89, 0x44, 0x24, 0x30, 0x4C,
        0x89, 0x4C, 0x24, 0x38, 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x40, 0xF3, 0x0F,
        0x7F, 0x4C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x60, 0xF3, 0x0F,
        0x7F, 0x5C, 0x24, 0x70, 0xF3, 0x0F, 0x7F, 0xA4, 0x24, 0x80, 0x00, 0x00,
        0x00, 0xF3, 0x0F, 0x7F, 0xAC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0xBB,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x31, 0xC0, 0xB9, 0x01,
        0x00, 0x00, 0x00, 0xF0, 0x0F, 0xB1, 0x0B, 0x0F, 0x85, 0xB4, 0x01, 0x00,
        0x00, 0xC7, 0x43, 0x04, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x08,
        0x48, 0x8B, 0x54, 0x24, 0x20, 0x48, 0x3B, 0x10, 0x0F, 0x85, 0x7F, 0x01,
        0x00, 0x00, 0x48, 0x8B, 0x42, 0x08, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x81,
        0x01, 0x00, 0x00, 0x48, 0x89, 0x84, 0x24, 0xA8, 0x00, 0x00, 0x00, 0x48,
        0x89, 0x83, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC1, 0xFF, 0x53, 0x18,
        0x89, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x89, 0x83, 0xA0, 0x00, 0x00,
        0x00, 0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x40,
        0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF,
        0xFF, 0x4C, 0x8D, 0x83, 0x88, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x28, 0x48,
        0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF,
        0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x48, 0x84, 0xC0, 0x75,
        0x67, 0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0xBA, 0x01, 0x00,
        0x00, 0x00, 0xFF, 0x53, 0x50, 0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00,
        0x00, 0x48, 0x8B, 0x53, 0x60, 0xFF, 0x53, 0x58, 0x48, 0x8B, 0x8C, 0x24,
        0xA8, 0x00, 0x00, 0x00, 0xBA, 0xFE, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x83,
        0x88, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x30, 0x48, 0x8B, 0x8C, 0x24, 0xA8,
        0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x83, 0x88,
        0x00, 0x00, 0x00, 0xFF, 0x53, 0x28, 0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00,
        0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0x41, 0xB8, 0x05, 0x00, 0x00,
        0x00, 0xFF, 0x53, 0x48, 0x84, 0xC0, 0x74, 0x7D, 0x48, 0x8B, 0x8C, 0x24,
        0xA8, 0x00, 0x00, 0x00, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x53, 0x38,
        0x48, 0x85, 0xC0, 0x74, 0x6F, 0x48, 0x89, 0x84, 0x24, 0xB0, 0x00, 0x00,
        0x00, 0x48, 0x89, 0x83, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x4B, 0x68,
        0x48, 0x8D, 0x94, 0x24, 0xA8, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x70, 0x48,
        0x85, 0xC0, 0x74, 0x53, 0x48, 0x89, 0xC1, 0x48, 0x8D, 0x94, 0x24, 0xB8,
        0x00, 0x00, 0x00, 0x4C, 0x8D, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0xFF,
        0x53, 0x78, 0x48, 0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0x8B, 0x94,
        0x24, 0xA0, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x20, 0x48, 0x8B, 0x8B, 0x80,
        0x00, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x10, 0x48, 0x89, 0x01, 0xC7, 0x43,
        0x04, 0x02, 0x00, 0x00, 0x00, 0xC7, 0x03, 0x02, 0x00, 0x00, 0x00, 0xEB,
        0x4C, 0xB9, 0x05, 0x00, 0x00, 0x00, 0xEB, 0x0C, 0xB9, 0x06, 0x00, 0x00,
        0x00, 0xEB, 0x05, 0xB9, 0x07, 0x00, 0x00, 0x00, 0x89, 0x4B, 0x04, 0x48,
        0x8B, 0x8C, 0x24, 0xA8, 0x00, 0x00, 0x00, 0x8B, 0x94, 0x24, 0xA0, 0x00,
        0x00, 0x00, 0xFF, 0x53, 0x20, 0xC7, 0x03, 0x00, 0x00, 0x00, 0x00, 0xEB,
        0x1C, 0xC7, 0x43, 0x04, 0x03, 0x00, 0x00, 0x00, 0xC7, 0x03, 0x00, 0x00,
        0x00, 0x00, 0xEB, 0x0D, 0xC7, 0x43, 0x04, 0x04, 0x00, 0x00, 0x00, 0xC7,
        0x03, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x40, 0xF3,
        0x0F, 0x6F, 0x4C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x60, 0xF3,
        0x0F, 0x6F, 0x5C, 0x24, 0x70, 0xF3, 0x0F, 0x6F, 0xA4, 0x24, 0x80, 0x00,
        0x00, 0x00, 0xF3, 0x0F, 0x6F, 0xAC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48,
        0x8B, 0x4C, 0x24, 0x20, 0x48, 0x8B, 0x54, 0x24, 0x28, 0x4C, 0x8B, 0x44,
        0x24, 0x30, 0x4C, 0x8B, 0x4C, 0x24, 0x38, 0x48, 0x8B, 0x43, 0x10, 0x48,
        0x81, 0xC4, 0xD0, 0x00, 0x00, 0x00, 0x5B, 0xFF, 0xE0,
}};

struct LuaRegistrarVector {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
};

bool SameEventVector(const EventVector& left, const EventVector& right) {
    return left.begin == right.begin && left.end == right.end &&
           left.capacity == right.capacity;
}

bool SameEventRecords(const std::vector<EventRecord>& left,
                      const std::vector<EventRecord>& right) {
    return left.size() == right.size() &&
           (left.empty() ||
            std::memcmp(left.data(), right.data(),
                        left.size() * sizeof(EventRecord)) == 0);
}

bool ReadEventVectorState(HANDLE process, std::uint64_t image_base,
                          EventVector& vector) {
    if (!ReadValue(process, image_base + kEventRecordsBeginRva,
                   vector.begin) ||
        !ReadValue(process, image_base + kEventRecordsEndRva, vector.end) ||
        !ReadValue(process, image_base + kEventRecordsCapacityRva,
                   vector.capacity))
        return false;
    if (!vector.begin)
        return !vector.end && !vector.capacity;
    if (vector.end < vector.begin || vector.capacity < vector.end)
        return false;
    const std::uint64_t used = vector.end - vector.begin;
    const std::uint64_t reserved = vector.capacity - vector.begin;
    return used % sizeof(EventRecord) == 0 &&
           reserved % sizeof(EventRecord) == 0 &&
           reserved / sizeof(EventRecord) <= 4096;
}

bool ReadEventRecords(HANDLE process, const EventVector& vector,
                      std::vector<EventRecord>& records) {
    records.clear();
    const std::size_t count = vector.begin
                                  ? static_cast<std::size_t>(
                                        (vector.end - vector.begin) /
                                        sizeof(EventRecord))
                                  : 0;
    records.resize(count);
    return records.empty() ||
           (ReadableRange(process, vector.begin,
                          records.size() * sizeof(EventRecord)) &&
            ReadExact(process, vector.begin, records.data(),
                      records.size() * sizeof(EventRecord)));
}

void CountEventRecords(const std::vector<EventRecord>& records,
                       std::uint64_t object, std::uint32_t index,
                       std::uint64_t original, std::size_t& slot_count,
                       std::size_t& exact_count,
                       std::size_t& exact_index) {
    slot_count = 0;
    exact_count = 0;
    exact_index = records.size();
    for (std::size_t position = 0; position != records.size(); ++position) {
        const auto& record = records[position];
        if (record.object != object || record.index != index)
            continue;
        ++slot_count;
        if (record.original == original) {
            ++exact_count;
            exact_index = position;
        }
    }
}

bool CaptureEventAppend(HANDLE process, std::uint64_t image_base,
                        EventState& state) {
    EventVector after{};
    std::vector<EventRecord> records;
    if (!ReadEventVectorState(process, image_base, after) ||
        !ReadEventRecords(process, after, records)) {
        state.registration_mutation_possible = true;
        return false;
    }
    if (!SameEventVector(after, state.vector_before) ||
        !SameEventRecords(records, state.records_before))
        state.registration_mutation_possible = true;
    if (records.size() != state.records_before.size() + 1 ||
        !std::equal(state.records_before.begin(), state.records_before.end(),
                    records.begin(), [](const EventRecord& left,
                                        const EventRecord& right) {
                        return std::memcmp(&left, &right,
                                           sizeof(EventRecord)) == 0;
                    }))
        return false;
    const auto& appended = records.back();
    if (appended.object != state.object ||
        appended.original != state.original ||
        appended.index != kEventHook.vtable_index)
        return false;
    state.vector_after = after;
    state.registration_record = after.end - sizeof(EventRecord);
    state.registration_changed = true;
    return true;
}

bool RollbackEventAppend(HANDLE process, std::uint64_t image_base,
                         EventState& state) {
    if (!state.registration_changed)
        return !state.registration_mutation_possible;
    EventVector current{};
    std::vector<EventRecord> records;
    if (!ReadEventVectorState(process, image_base, current) ||
        !SameEventVector(current, state.vector_after) ||
        !ReadEventRecords(process, current, records) ||
        records.size() != state.records_before.size() + 1 ||
        !std::equal(state.records_before.begin(), state.records_before.end(),
                    records.begin(), [](const EventRecord& left,
                                        const EventRecord& right) {
                        return std::memcmp(&left, &right,
                                           sizeof(EventRecord)) == 0;
                    }))
        return false;
    const auto& appended = records.back();
    if (appended.object != state.object ||
        appended.original != state.original ||
        appended.index != kEventHook.vtable_index ||
        state.registration_record != current.end - sizeof(EventRecord) ||
        !WriteProtectedQword(process,
                             image_base + kEventRecordsEndRva,
                             state.registration_record))
        return false;
    EventVector restored{};
    std::vector<EventRecord> restored_records;
    if (!ReadEventVectorState(process, image_base, restored) ||
        restored.begin != current.begin ||
        restored.end != state.registration_record ||
        restored.capacity != current.capacity ||
        !ReadEventRecords(process, restored, restored_records) ||
        !SameEventRecords(restored_records, state.records_before))
        return false;
    state.registration_changed = false;
    state.registration_mutation_possible = false;
    return true;
}

bool EventRegistrationExact(HANDLE process, std::uint64_t image_base,
                            const EventState& state) {
    EventVector current{};
    std::vector<EventRecord> records;
    const EventVector& expected = state.registration_changed
                                      ? state.vector_after
                                      : state.vector_before;
    if (!ReadEventVectorState(process, image_base, current) ||
        !SameEventVector(current, expected) ||
        !ReadEventRecords(process, current, records))
        return false;
    if (!state.registration_changed)
        return SameEventRecords(records, state.records_before);
    if (records.size() != state.records_before.size() + 1 ||
        !std::equal(state.records_before.begin(), state.records_before.end(),
                    records.begin(), [](const EventRecord& left,
                                        const EventRecord& right) {
                        return std::memcmp(&left, &right,
                                           sizeof(EventRecord)) == 0;
                    }))
        return false;
    const auto& appended = records.back();
    return appended.object == state.object &&
           appended.original == state.original &&
           appended.index == kEventHook.vtable_index &&
           state.registration_record == current.end - sizeof(EventRecord);
}

bool ReadVtable(HANDLE process, std::uint64_t vtable,
                std::uint32_t required_index,
                std::vector<std::uint64_t>& entries, Failure& failure,
                const char* stage, std::size_t index) {
    constexpr std::size_t kMaximumEntries = 4096;
    entries.clear();
    entries.reserve(256);
    for (std::size_t entry_index = 0; entry_index != kMaximumEntries;
         ++entry_index) {
        if (entry_index > (UINT64_MAX - vtable) / sizeof(std::uint64_t))
            return Fail(failure, stage, "vtable_address", index, vtable,
                        entry_index, kMaximumEntries);
        std::uint64_t entry = 0;
        const std::uint64_t address =
            vtable + entry_index * sizeof(std::uint64_t);
        if (!ReadableRange(process, address, sizeof(entry)) ||
            !ReadValue(process, address, entry))
            return Fail(failure, stage, "vtable_read", index, address,
                        entry_index, kMaximumEntries, GetLastError());
        entries.push_back(entry);
        if (!entry) {
            if (required_index >= entry_index)
                return Fail(failure, stage, "vtable_index", index, vtable,
                            required_index, entry_index);
            return true;
        }
    }
    return Fail(failure, stage, "vtable_sentinel", index, vtable,
                kMaximumEntries, 0);
}

bool ValidateCloneInfrastructure(HANDLE process, std::uint64_t image_base,
                                 Failure& failure) {
    struct Entry {
        std::uint32_t rva;
        const std::uint8_t* prefix;
    };
    static constexpr std::array<Entry, 12> entries{{
        {0x2F3A20, kCloneConstructorPrefix},
        {0x49BEE4, kCloneAllocatorPrefix},
        {0x2F3300, kCloneCleanupPrefix},
        {0x2F4340, kInstaller2f4340Prefix},
        {0x2FF160, kInstaller2ff160Prefix},
        {0x31B300, kInstaller31b300Prefix},
        {0x33D620, kEventInstallerPrefix},
        {0x31B0D0, kInstaller31b0d0Prefix},
        {0x31B210, kInstaller31b210Prefix},
        {0x3010C0, kInstaller3010c0Prefix},
        {0x31C430, kInstaller31c430Prefix},
        {0x349AD0, kInstaller349ad0Prefix},
    }};
    for (std::size_t index = 0; index != entries.size(); ++index) {
        const std::uint64_t address = image_base + entries[index].rva;
        if (!ExecutableRange(process, address, kDetourPrefixSize) ||
            !VerifyBytes(process, address, entries[index].prefix,
                         kDetourPrefixSize, failure, "clone_infrastructure",
                         "entry_prefix", index))
            return false;
    }
    return true;
}

bool ValidateCloneRecord(HANDLE process, std::uint64_t record_address,
                         std::uint64_t object,
                         std::uint64_t expected_original_vtable,
                         std::uint32_t vtable_index,
                         std::uint64_t detour, std::uint64_t saved,
                         const std::vector<std::uint64_t>* source,
                         bool patched, CloneRecord& record, Failure& failure,
                         const char* stage, std::size_t index) {
    if (!record_address ||
        !ReadableRange(process, record_address, sizeof(record)) ||
        !ReadValue(process, record_address, record))
        return Fail(failure, stage, "record_read", index, record_address,
                    0, sizeof(record), GetLastError());
    if (record.original_vtable != expected_original_vtable ||
        !record.cloned_vtable ||
        record.object != object || record.index != vtable_index ||
        record.detour != detour)
        return Fail(failure, stage, "record_exact", index, record_address,
                    record.object, object);
    std::uint64_t original_entry = 0;
    std::uint64_t cloned_entry = 0;
    const std::uint64_t original_slot =
        record.original_vtable + static_cast<std::uint64_t>(vtable_index) * 8;
    const std::uint64_t cloned_slot =
        record.cloned_vtable + static_cast<std::uint64_t>(vtable_index) * 8;
    if (!ReadValue(process, original_slot, original_entry) ||
        !ReadValue(process, cloned_slot, cloned_entry))
        return Fail(failure, stage, "record_entries_read", index,
                    record_address, 0, 0, GetLastError());
    const std::uint64_t expected_clone = patched ? detour : original_entry;
    if (original_entry != saved || cloned_entry != expected_clone)
        return Fail(failure, stage, "record_entries_exact", index,
                    cloned_slot, cloned_entry, expected_clone);
    if (source) {
        std::vector<std::uint64_t> clone(source->size());
        if (!ReadableRange(process, record.cloned_vtable,
                           clone.size() * sizeof(std::uint64_t)) ||
            !ReadExact(process, record.cloned_vtable, clone.data(),
                       clone.size() * sizeof(std::uint64_t)))
            return Fail(failure, stage, "clone_read", index,
                        record.cloned_vtable, 0, clone.size(),
                        GetLastError());
        for (std::size_t entry_index = 0; entry_index != clone.size();
             ++entry_index) {
            const std::uint64_t expected =
                entry_index == vtable_index ? expected_clone
                                            : (*source)[entry_index];
            if (clone[entry_index] != expected)
                return Fail(failure, stage, "clone_copy_exact", index,
                            record.cloned_vtable + entry_index * 8,
                            clone[entry_index], expected);
        }
    }
    return true;
}

bool PrepareCloneHooks(
    HANDLE process, std::uint64_t image_base, const ModuleInfo& client,
    const ModuleInfo& engine, const ModuleInfo& lua_shared,
    const ModuleInfo& vgui2,
    std::array<CloneState, kCloneHooks.size()>& states,
    std::size_t& missing_generic_count, Failure& failure) {
    if (!ValidateCloneInfrastructure(process, image_base, failure))
        return false;
    static constexpr std::array<std::uint32_t, 4> group_slots{
        0x7EA7D8, 0x7EA800, 0x7EA810, 0x7EA878};
    std::array<bool, group_slots.size()> missing_seen{};
    missing_generic_count = 0;
    for (std::size_t index = 0; index != kCloneHooks.size(); ++index) {
        const auto& spec = kCloneHooks[index];
        auto& state = states[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        const std::uint64_t object_slot = image_base + spec.object_slot_rva;
        std::uint64_t vtable = 0;
        std::uint64_t live = 0;
        std::uint64_t saved = 0;
        if (!ReadValue(process, object_slot, state.object) || !state.object ||
            !ReadValue(process, state.object, vtable) || !vtable ||
            !ReadValue(process,
                       vtable + static_cast<std::uint64_t>(spec.vtable_index) * 8,
                       live) ||
            !ReadValue(process, image_base + spec.saved_slot_rva, saved))
            return Fail(failure, "clone_pre", "object_entry_saved", index,
                        object_slot, state.object, 1, GetLastError());
        const std::uint64_t detour = image_base + spec.detour_rva;
        if (!ExecutableRange(process, detour, kDetourPrefixSize) ||
            !VerifyBytes(process, detour, spec.detour_prefix,
                         kDetourPrefixSize, failure, "clone_pre",
                         "detour_prefix", index))
            return false;
        std::size_t group = group_slots.size();
        for (std::size_t candidate = 0; candidate != group_slots.size();
             ++candidate) {
            if (group_slots[candidate] == spec.object_slot_rva) {
                group = candidate;
                break;
            }
        }
        if (group == group_slots.size())
            return Fail(failure, "clone_pre", "known_group", index,
                        object_slot, spec.object_slot_rva, 0);
        if (live == detour && saved &&
            RangeInModule(saved, 1, module) &&
            ExecutableRange(process, saved)) {
            if (missing_seen[group])
                return Fail(failure, "clone_pre", "installed_prefix", index,
                            object_slot, spec.vtable_index, group);
            state.installed = true;
            state.original = saved;
        } else {
            if (live != detour && saved == live &&
                RangeInModule(live, 1, module) &&
                ExecutableRange(process, live)) {
                if (!WriteProtectedQword(process,
                                         image_base + spec.saved_slot_rva, 0))
                    return Fail(failure, "clone_pre", "precommit_clear",
                                index, image_base + spec.saved_slot_rva,
                                saved, 0, GetLastError());
                saved = 0;
            }
            if (live == detour || saved != 0 ||
                !RangeInModule(live, 1, module) ||
                !ExecutableRange(process, live))
                return Fail(failure, "clone_pre", "layer_consistent", index,
                            vtable + static_cast<std::uint64_t>(
                                         spec.vtable_index) * 8,
                            live, detour);
            missing_seen[group] = true;
            state.original = live;
            std::vector<std::uint64_t> entries;
            if (!ReadVtable(process, vtable, spec.vtable_index, entries,
                            failure, "clone_pre", index))
                return false;
            if (spec.form == CloneInstallForm::Generic)
                ++missing_generic_count;
        }
        if (spec.form == CloneInstallForm::Static) {
            state.owner = image_base + spec.owner_rva;
            std::uint64_t owner_vtable = 0;
            std::uint64_t owner_record = 0;
            if (!ReadValue(process, state.owner, owner_vtable) ||
                !ReadValue(process, state.owner + 8, owner_record))
                return Fail(failure, "clone_pre", "owner_read", index,
                            state.owner, 0, 0, GetLastError());
            const std::uint64_t expected_owner_vtable =
                image_base + spec.owner_vtable_rva;
            if (owner_vtable != expected_owner_vtable)
                return Fail(failure, "clone_pre", "owner_vtable", index,
                            state.owner, owner_vtable,
                            expected_owner_vtable);
            if (!state.installed && owner_record)
                return Fail(failure, "clone_pre", "owner_empty", index,
                            state.owner + 8, owner_record, 0);
            if (state.installed) {
                if (!owner_record)
                    return Fail(failure, "clone_pre", "owner_record", index,
                                state.owner + 8, 0, 1);
                CloneRecord record{};
                std::vector<std::uint64_t> source;
                if (!ReadValue(process, owner_record, record))
                    return Fail(failure, "clone_pre", "owner_record_read",
                                index, owner_record, 0, sizeof(record),
                                GetLastError());
                if (!ReadVtable(process, record.original_vtable,
                                spec.vtable_index, source, failure,
                                "clone_pre", index) ||
                    !ValidateCloneRecord(
                        process, owner_record, state.object,
                        record.original_vtable, spec.vtable_index, detour,
                        state.original, &source, true, record, failure,
                        "clone_pre", index))
                    return false;
                state.record = owner_record;
                state.previous_vtable = record.original_vtable;
                state.cloned_vtable = record.cloned_vtable;
            }
        }
    }
    return true;
}

bool VerifyCloneGroupThrough(
    HANDLE process, std::uint64_t image_base,
    const std::array<CloneState, kCloneHooks.size()>& states,
    std::size_t through, Failure& failure) {
    const auto& current_spec = kCloneHooks[through];
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    if (!ReadValue(process, image_base + current_spec.object_slot_rva,
                   object) ||
        object != states[through].object ||
        !ReadValue(process, object, vtable) || !vtable)
        return Fail(failure, "clone_post", "group_object", through,
                    image_base + current_spec.object_slot_rva, object,
                    states[through].object, GetLastError());
    for (std::size_t index = 0; index <= through; ++index) {
        const auto& spec = kCloneHooks[index];
        const auto& state = states[index];
        if (spec.object_slot_rva != current_spec.object_slot_rva ||
            (!state.installed && !state.mutation_possible))
            continue;
        std::uint64_t live = 0;
        std::uint64_t saved = 0;
        const std::uint64_t detour = image_base + spec.detour_rva;
        if (!ReadValue(process,
                       vtable + static_cast<std::uint64_t>(spec.vtable_index) * 8,
                       live) ||
            !ReadValue(process, image_base + spec.saved_slot_rva, saved) ||
            live != detour || saved != state.original)
            return Fail(failure, "clone_post", "group_layer", index,
                        vtable + static_cast<std::uint64_t>(
                                     spec.vtable_index) * 8,
                        live, detour, GetLastError());
    }
    return true;
}

bool ApplyCloneHook(HANDLE process, std::uint64_t image_base,
                    const CloneHookSpec& spec, CloneState& state,
                    Failure& failure, std::size_t index) {
    if (state.installed)
        return true;
    std::uint64_t object = 0;
    if (!ReadValue(process, image_base + spec.object_slot_rva, object) ||
        object != state.object ||
        !ReadValue(process, object, state.previous_vtable) ||
        !state.previous_vtable)
        return Fail(failure, "clone_apply", "object_stable", index,
                    image_base + spec.object_slot_rva, object, state.object,
                    GetLastError());
    if (!ReadVtable(process, state.previous_vtable, spec.vtable_index,
                    state.source_entries, failure, "clone_apply", index))
        return false;
    if (state.source_entries[spec.vtable_index] != state.original)
        return Fail(failure, "clone_apply", "original_stable", index,
                    state.previous_vtable +
                        static_cast<std::uint64_t>(spec.vtable_index) * 8,
                    state.source_entries[spec.vtable_index], state.original);
    const std::uint64_t detour = image_base + spec.detour_rva;
    if (spec.form == CloneInstallForm::Generic) {
        std::uint64_t allocated = 0;
        if (!RemoteCall4(process, image_base + 0x49BEE4, 0x28, 0, 0, 0,
                         allocated, failure, "clone_allocate", index))
            return false;
        if (!allocated || !WritableRange(process, allocated, sizeof(CloneRecord)))
            return Fail(failure, "clone_allocate", "record_writable", index,
                        allocated, 0, sizeof(CloneRecord));
        state.record = allocated;
        if (!WriteProtectedQword(process, state.owner + 8, state.record))
            return Fail(failure, "clone_apply", "owner_record_write", index,
                        state.owner + 8, 0, state.record, GetLastError());
        state.owner_record_changed = true;
        std::uint64_t result = UINT64_MAX;
        state.mutation_possible = true;
        if (!RemoteCall4(process, image_base + 0x2F3A20, state.record,
                         state.object, spec.vtable_index, detour, result,
                         failure, "clone_construct", index))
            return false;
        CloneRecord record{};
        if (!ValidateCloneRecord(
                process, state.record, state.object, state.previous_vtable,
                spec.vtable_index,
                detour, state.original, &state.source_entries, false, record,
                failure, "clone_construct", index))
            return false;
        state.cloned_vtable = record.cloned_vtable;
        std::uint64_t active_vtable = 0;
        if (!ReadValue(process, state.object, active_vtable) ||
            active_vtable != state.cloned_vtable)
            return Fail(failure, "clone_construct", "published_vtable",
                        index, state.object, active_vtable,
                        state.cloned_vtable, GetLastError());
        state.cleanup_safe = true;
        if (result != state.record)
            return Fail(failure, "clone_construct", "result_record", index,
                        image_base + 0x2F3A20, result, state.record);
        if (!WriteProtectedQword(process, image_base + spec.saved_slot_rva,
                                 state.original))
            return Fail(failure, "clone_apply", "saved_write", index,
                        image_base + spec.saved_slot_rva, 0, state.original,
                        GetLastError());
        state.saved_changed = true;
        if (!WriteProtectedQword(
                process,
                state.cloned_vtable +
                    static_cast<std::uint64_t>(spec.vtable_index) * 8,
                detour))
            return Fail(failure, "clone_apply", "entry_write", index,
                        state.cloned_vtable +
                            static_cast<std::uint64_t>(spec.vtable_index) * 8,
                        0, detour, GetLastError());
    } else {
        std::uint64_t result = UINT64_MAX;
        state.mutation_possible = true;
        state.owner_record_changed = true;
        state.saved_changed = true;
        if (!RemoteCall3(process, image_base + spec.installer_rva,
                         state.owner, 0, 0, result, failure,
                         "clone_static_call", index))
            return false;
        if (!ReadValue(process, state.owner + 8, state.record) ||
            !state.record)
            return Fail(failure, "clone_static", "owner_record", index,
                        state.owner + 8, state.record, 1, GetLastError());
        CloneRecord record{};
        if (!ValidateCloneRecord(
                process, state.record, state.object, state.previous_vtable,
                spec.vtable_index,
                detour, state.original, &state.source_entries, true, record,
                failure, "clone_static", index))
            return false;
        state.cloned_vtable = record.cloned_vtable;
        std::uint64_t active_vtable = 0;
        std::uint64_t saved = 0;
        if (!ReadValue(process, state.object, active_vtable) ||
            !ReadValue(process, image_base + spec.saved_slot_rva, saved) ||
            active_vtable != state.cloned_vtable || saved != state.original)
            return Fail(failure, "clone_static", "published_exact", index,
                        state.object, active_vtable, state.cloned_vtable,
                        GetLastError());
        state.cleanup_safe = true;
    }
    return true;
}

bool VerifyCloneHooks(
    HANDLE process, std::uint64_t image_base, const ModuleInfo& client,
    const ModuleInfo& engine, const ModuleInfo& lua_shared,
    const ModuleInfo& vgui2,
    const std::array<CloneState, kCloneHooks.size()>& states,
    Failure& failure) {
    for (std::size_t index = 0; index != kCloneHooks.size(); ++index) {
        const auto& spec = kCloneHooks[index];
        const auto& state = states[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        std::uint64_t object = 0;
        std::uint64_t vtable = 0;
        std::uint64_t live = 0;
        std::uint64_t saved = 0;
        const std::uint64_t detour = image_base + spec.detour_rva;
        if (!ReadValue(process, image_base + spec.object_slot_rva, object) ||
            object != state.object || !ReadValue(process, object, vtable) ||
            !ReadValue(process,
                       vtable + static_cast<std::uint64_t>(spec.vtable_index) * 8,
                       live) ||
            !ReadValue(process, image_base + spec.saved_slot_rva, saved))
            return Fail(failure, "clone_post", "layer_read", index,
                        image_base + spec.object_slot_rva, object,
                        state.object, GetLastError());
        if (index == kSkippedCreateMoveCloneIndex) {
            if (live != state.original || saved != 0 ||
                !RangeInModule(live, 1, module) ||
                !ExecutableRange(process, live))
                return Fail(failure, "clone_post", "createmove_original",
                            index,
                            vtable + static_cast<std::uint64_t>(
                                         spec.vtable_index) * 8,
                            live, state.original, GetLastError());
            continue;
        }
        if (live != detour || saved != state.original ||
            !RangeInModule(saved, 1, module) ||
            !ExecutableRange(process, saved) ||
            !VerifyBytes(process, detour, spec.detour_prefix,
                         kDetourPrefixSize, failure, "clone_post",
                         "detour_prefix", index))
            return Fail(failure, "clone_post", "layer_exact", index,
                        vtable + static_cast<std::uint64_t>(
                                     spec.vtable_index) * 8,
                        live, detour, GetLastError());
    }
    return true;
}

bool RollbackCloneHooks(
    HANDLE process, std::uint64_t image_base,
    std::array<CloneState, kCloneHooks.size()>& states,
    void* generic_owner_page) {
    bool ok = true;
    Failure ignored{};
    for (std::size_t reverse = states.size(); reverse != 0; --reverse) {
        const std::size_t index = reverse - 1;
        auto& state = states[index];
        if (!state.mutation_possible && !state.owner_record_changed &&
            !state.saved_changed)
            continue;
        bool detached = !state.mutation_possible;
        if (state.cleanup_safe) {
            std::uint64_t result = UINT64_MAX;
            const bool called =
                RemoteCall3(process, image_base + 0x2F3300, state.owner,
                            0, 0, result, ignored, "rollback_clone", index);
            if (!called && !RemoteCallsRollbackSafe())
                return false;
            if (called) {
                std::uint64_t active_vtable = 0;
                detached = ReadValue(process, state.object, active_vtable) &&
                           active_vtable == state.previous_vtable;
            }
        }
        if (!detached && state.mutation_possible)
            detached = WriteProtectedQword(process, state.object,
                                           state.previous_vtable);
        if (!detached)
            ok = false;
        if (detached && state.saved_changed &&
            !WriteProtectedQword(
                process,
                image_base + kCloneHooks[index].saved_slot_rva, 0))
            ok = false;
        if (state.owner_record_changed && state.owner &&
            !WriteProtectedQword(process, state.owner + 8, 0))
            ok = false;
    }
    if (generic_owner_page &&
        !VirtualFreeEx(process, generic_owner_page, 0, MEM_RELEASE))
        ok = false;
    return ok;
}

bool PrepareEventHook(HANDLE process, std::uint64_t image_base,
                      const ModuleInfo& engine, EventState& state,
                      Failure& failure) {
    const std::uint64_t object_slot = image_base + kEventHook.object_slot_rva;
    std::uint64_t live = 0;
    std::uint64_t saved = 0;
    if (!ReadValue(process, object_slot, state.object) || !state.object ||
        !ReadValue(process, state.object, state.vtable) || !state.vtable) {
        return Fail(failure, "event_pre", "object_vtable", 0, object_slot,
                    state.object, 1, GetLastError());
    }
    state.entry_slot = state.vtable +
                       static_cast<std::uint64_t>(kEventHook.vtable_index) * 8;
    if (!ReadValue(process, state.entry_slot, live) ||
        !ReadValue(process, image_base + kEventHook.saved_slot_rva, saved))
        return Fail(failure, "event_pre", "entry_saved", 0,
                    state.entry_slot, 0, 0, GetLastError());
    if (!ReadEventVectorState(process, image_base, state.vector_before) ||
        !ReadEventRecords(process, state.vector_before,
                          state.records_before))
        return Fail(failure, "event_pre", "registration_vector", 0,
                    image_base + kEventRecordsBeginRva, 0, 1,
                    GetLastError());
    const std::uint64_t detour = image_base + kEventHook.detour_rva;
    if (!ExecutableRange(process, detour, kDetourPrefixSize) ||
        !VerifyBytes(process, detour, kEventHook.detour_prefix,
                     kDetourPrefixSize, failure, "event_pre",
                     "detour_prefix", 0))
        return false;
    if (live == detour && saved && RangeInModule(saved, 1, engine) &&
        ExecutableRange(process, saved)) {
        std::size_t slot_count = 0;
        std::size_t exact_count = 0;
        std::size_t exact_index = state.records_before.size();
        CountEventRecords(state.records_before, state.object,
                          kEventHook.vtable_index, saved, slot_count,
                          exact_count, exact_index);
        if (slot_count != 1 || exact_count != 1 ||
            exact_index + 1 != state.records_before.size())
            return Fail(failure, "event_pre", "registration_exact", 0,
                        image_base + kEventRecordsEndRva, exact_count, 1);
        state.installed = true;
        state.original = saved;
        return true;
    }
    if (live != detour && saved == live &&
        RangeInModule(live, 1, engine) && ExecutableRange(process, live)) {
        std::size_t slot_count = 0;
        std::size_t exact_count = 0;
        std::size_t exact_index = state.records_before.size();
        CountEventRecords(state.records_before, state.object,
                          kEventHook.vtable_index, live, slot_count,
                          exact_count, exact_index);
        if (slot_count)
            return Fail(failure, "event_pre", "pending_registration_empty",
                        0, image_base + kEventRecordsBeginRva, slot_count, 0);
        state.pending = true;
        state.original = live;
        return true;
    }
    if (live == detour || saved || !RangeInModule(live, 1, engine) ||
        !ExecutableRange(process, live))
        return Fail(failure, "event_pre", "layer_consistent", 0,
                    state.entry_slot, live, detour);
    state.original = live;
    std::size_t slot_count = 0;
    std::size_t exact_count = 0;
    std::size_t exact_index = state.records_before.size();
    CountEventRecords(state.records_before, state.object,
                      kEventHook.vtable_index, state.original, slot_count,
                      exact_count, exact_index);
    if (slot_count)
        return Fail(failure, "event_pre", "registration_empty", 0,
                    image_base + kEventRecordsBeginRva, slot_count, 0);
    return true;
}

bool VerifyEventHook(HANDLE process, std::uint64_t image_base,
                     const ModuleInfo& engine, const EventState& state,
                     Failure& failure, const char* stage) {
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t live = 0;
    std::uint64_t saved = 0;
    const std::uint64_t detour = image_base + kEventHook.detour_rva;
    if (!ReadValue(process, image_base + kEventHook.object_slot_rva, object) ||
        object != state.object || !ReadValue(process, object, vtable) ||
        !ReadValue(process,
                   vtable + static_cast<std::uint64_t>(
                                kEventHook.vtable_index) * 8,
                   live) ||
        !ReadValue(process, image_base + kEventHook.saved_slot_rva, saved) ||
        live != detour || saved != state.original ||
        !RangeInModule(saved, 1, engine) || !ExecutableRange(process, saved) ||
        !VerifyBytes(process, detour, kEventHook.detour_prefix,
                     kDetourPrefixSize, failure, stage, "detour_prefix", 0))
        return Fail(failure, stage, "layer_exact", 0,
                    vtable + static_cast<std::uint64_t>(
                                 kEventHook.vtable_index) * 8,
                    live, detour, GetLastError());
    if (!EventRegistrationExact(process, image_base, state))
        return Fail(failure, stage, "registration_exact", 0,
                    image_base + kEventRecordsEndRva, 0, 1,
                    GetLastError());
    return true;
}

bool ApplyEventHook(HANDLE process, std::uint64_t image_base,
                    const ModuleInfo& engine, EventState& state,
                    Failure& failure) {
    if (state.installed)
        return VerifyEventHook(process, image_base, engine, state, failure,
                               "event_post");
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    std::uint64_t live = 0;
    if (!ReadValue(process, image_base + kEventHook.object_slot_rva, object) ||
        object != state.object || !ReadValue(process, object, vtable) ||
        vtable != state.vtable ||
        !ReadValue(process, state.entry_slot, live) ||
        live != state.original)
        return Fail(failure, "event_apply", "target_stable", 0,
                    image_base + kEventHook.object_slot_rva, object,
                    state.object, GetLastError());
    EventVector current_vector{};
    std::vector<EventRecord> current_records;
    if (!ReadEventVectorState(process, image_base, current_vector) ||
        !SameEventVector(current_vector, state.vector_before) ||
        !ReadEventRecords(process, current_vector, current_records) ||
        !SameEventRecords(current_records, state.records_before))
        return Fail(failure, "event_apply", "registration_stable", 0,
                    image_base + kEventRecordsBeginRva, current_vector.end,
                    state.vector_before.end, GetLastError());
    const std::uint64_t saved_slot =
        image_base + kEventHook.saved_slot_rva;
    if (!WriteProtectedQword(process, saved_slot, state.original))
        return Fail(failure, "event_apply", "saved_write", 0, saved_slot,
                    0, state.original, GetLastError());
    state.saved_changed = true;
    std::uint64_t result = UINT64_MAX;
    const bool called =
        RemoteCall4(process, image_base + 0x33D620, 0,
                    image_base + kEventHook.detour_rva,
                    kEventHook.vtable_index, 0, result, failure,
                    "event_call", 0);
    if (!called) {
        std::uint64_t live = 0;
        if (ReadValue(process, state.entry_slot, live) &&
            live == image_base + kEventHook.detour_rva)
            state.mutation_possible = true;
        CaptureEventAppend(process, image_base, state);
        return false;
    }
    state.mutation_possible = true;
    if (!CaptureEventAppend(process, image_base, state))
        return Fail(failure, "event_apply", "registration_append", 0,
                    image_base + kEventRecordsEndRva,
                    state.vector_after.end, state.vector_before.end);
    if (result != state.original)
        return Fail(failure, "event_apply", "returned_original", 0,
                    image_base + 0x33D620, result, state.original);
    return VerifyEventHook(process, image_base, engine, state, failure,
                           "event_post");
}

bool RollbackEventHook(HANDLE process, std::uint64_t image_base,
                       EventState& state) {
    bool ok = true;
    bool detached = !state.mutation_possible;
    if (state.mutation_possible)
        detached = WriteProtectedQword(process, state.entry_slot,
                                       state.original);
    if (!detached)
        ok = false;
    if (detached && !RollbackEventAppend(process, image_base, state))
        ok = false;
    if (detached && state.saved_changed &&
        !WriteProtectedQword(process,
                             image_base + kEventHook.saved_slot_rva, 0))
        ok = false;
    return ok;
}

bool LuaRealmVtableAccepted(const LuaBootstrapState& state,
                            std::uint64_t value) {
    for (std::size_t index = 0; index != state.realm_vtable_count; ++index) {
        if (state.realm_vtables[index] == value)
            return true;
    }
    return false;
}

bool ResolveLuaRealmVtables(HANDLE process, const ModuleInfo& lua_shared,
                            std::uint64_t original,
                            std::uint64_t detour,
                            std::uint64_t required_vtable,
                            LuaBootstrapState& state, Failure& failure) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadValue(process, lua_shared.base, dos))
        return Fail(failure, "lua_topology", "dos_read", 0,
                    lua_shared.base, 0, 0, GetLastError());
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::uint64_t>(dos.e_lfanew) > lua_shared.size ||
        sizeof(IMAGE_NT_HEADERS64) >
            lua_shared.size - static_cast<std::uint64_t>(dos.e_lfanew))
        return Fail(failure, "lua_topology", "dos_exact", 0,
                    lua_shared.base, dos.e_magic, IMAGE_DOS_SIGNATURE);
    const std::uint64_t nt_address =
        lua_shared.base + static_cast<std::uint32_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(process, nt_address, nt))
        return Fail(failure, "lua_topology", "nt_read", 0,
                    nt_address, 0, 0, GetLastError());
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage != lua_shared.size)
        return Fail(failure, "lua_topology", "nt_exact", 0,
                    nt_address, nt.OptionalHeader.SizeOfImage,
                    lua_shared.size);
    const std::uint64_t section_address =
        nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (section_address < nt_address ||
        section_address - lua_shared.base > lua_shared.size ||
        section_bytes >
            lua_shared.size - (section_address - lua_shared.base))
        return Fail(failure, "lua_topology", "sections_range", 0,
                    section_address, section_bytes, lua_shared.size);
    std::vector<IMAGE_SECTION_HEADER> sections(
        nt.FileHeader.NumberOfSections);
    if (!ReadExact(process, section_address, sections.data(), section_bytes))
        return Fail(failure, "lua_topology", "sections_read", 0,
                    section_address, 0, section_bytes, GetLastError());
    state.realm_vtables.fill(0);
    state.realm_vtable_count = 0;
    bool found_rdata = false;
    constexpr std::size_t entry_count = 132;
    constexpr std::size_t run_string_index = 111;
    constexpr std::size_t table_bytes =
        entry_count * sizeof(std::uint64_t);
    for (const auto& section : sections) {
        if (std::memcmp(section.Name, ".rdata", 6) != 0 ||
            section.Name[6] != 0)
            continue;
        found_rdata = true;
        std::uint64_t extent = section.Misc.VirtualSize;
        if (!extent)
            extent = section.SizeOfRawData;
        const std::uint64_t rva = section.VirtualAddress;
        if (!extent || rva >= lua_shared.size ||
            extent > lua_shared.size - rva)
            return Fail(failure, "lua_topology", "rdata_range", 0,
                        lua_shared.base + rva, extent,
                        lua_shared.size - rva);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent));
        if (!ReadExact(process, lua_shared.base + rva, bytes.data(),
                       bytes.size()))
            return Fail(failure, "lua_topology", "rdata_read", 0,
                        lua_shared.base + rva, 0, bytes.size(),
                        GetLastError());
        for (std::size_t offset = 0; offset + table_bytes <= bytes.size();
             offset += sizeof(std::uint64_t)) {
            std::uint64_t run_string = 0;
            std::memcpy(&run_string,
                        bytes.data() + offset +
                            run_string_index * sizeof(std::uint64_t),
                        sizeof(run_string));
            if (run_string != original && run_string != detour)
                continue;
            bool valid = true;
            for (std::size_t index = 0; index != entry_count; ++index) {
                std::uint64_t entry = 0;
                std::memcpy(&entry,
                            bytes.data() + offset +
                                index * sizeof(std::uint64_t),
                            sizeof(entry));
                const bool accepted_detour =
                    index == run_string_index && entry == detour;
                if ((!accepted_detour &&
                     !RangeInModule(entry, 1, lua_shared)) ||
                    !ExecutableRange(process, entry)) {
                    valid = false;
                    break;
                }
            }
            if (!valid)
                continue;
            const std::uint64_t candidate =
                lua_shared.base + rva + offset;
            bool duplicate = false;
            for (std::size_t index = 0;
                 index != state.realm_vtable_count; ++index) {
                if (state.realm_vtables[index] == candidate)
                    duplicate = true;
            }
            if (duplicate)
                continue;
            if (state.realm_vtable_count == state.realm_vtables.size())
                return Fail(failure, "lua_topology", "vtable_count", 0,
                            candidate, state.realm_vtable_count + 1,
                            state.realm_vtables.size());
            state.realm_vtables[state.realm_vtable_count++] = candidate;
        }
    }
    if (!found_rdata || state.realm_vtable_count == 0 ||
        !LuaRealmVtableAccepted(state, required_vtable))
        return Fail(failure, "lua_topology", "vtable_unique", 0,
                    required_vtable, state.realm_vtable_count, 1);
    return true;
}

bool ResolveLuaRunStringOriginal(HANDLE process,
                                 const ModuleInfo& lua_shared,
                                 std::uint64_t& original,
                                 Failure& failure) {
    std::vector<ExecutableSection> sections;
    if (!LoadExecutableSections(process, lua_shared, sections, failure, 0))
        return false;
    original = 0;
    std::size_t count = 0;
    for (const auto& section : sections) {
        for (std::size_t offset = 0;
             offset + sizeof(kLuaRunStringOriginalPrefix) <=
                 section.bytes.size();
             ++offset) {
            if (std::memcmp(section.bytes.data() + offset,
                            kLuaRunStringOriginalPrefix,
                            sizeof(kLuaRunStringOriginalPrefix)) != 0)
                continue;
            original = lua_shared.base + section.rva + offset;
            ++count;
        }
    }
    if (count != 1)
        return Fail(failure, "lua_topology", "run_string_unique", 0,
                    original ? original : lua_shared.base, count, 1);
    return true;
}

bool PrepareLuaBootstrap(HANDLE process, std::uint64_t image_base,
                         const ModuleInfo& lua_shared,
                         std::uint64_t shared,
                         LuaBootstrapState& state,
                         Failure& failure) {
    std::uint64_t shared_vtable = 0;
    std::uint64_t get_lua = 0;
    std::uint64_t run_string_original = 0;
    if (!shared || !ReadValue(process, shared, shared_vtable) ||
        !RangeInModule(shared_vtable, 19 * sizeof(std::uint64_t),
                       lua_shared) ||
        !ReadableRange(process, shared_vtable,
                       19 * sizeof(std::uint64_t)))
        return Fail(failure, "lua_bootstrap", "shared_vtable", 0, shared,
                    shared_vtable, lua_shared.base,
                    GetLastError());
    if (!ResolveLuaRunStringOriginal(process, lua_shared,
                                     run_string_original, failure) ||
        !RangeInModule(run_string_original,
                       sizeof(kLuaRunStringOriginalPrefix), lua_shared) ||
        !ExecutableRange(process, run_string_original) ||
        !VerifyBytes(process, run_string_original,
                     kLuaRunStringOriginalPrefix,
                     sizeof(kLuaRunStringOriginalPrefix), failure,
                     "lua_bootstrap", "run_string_original", 0))
        return false;
    state.shared_vtable = shared_vtable;
    state.original = run_string_original;
    std::array<std::uint64_t, 19> shared_entries{};
    if (!ReadExact(process, shared_vtable, shared_entries.data(),
                   sizeof(shared_entries)))
        return Fail(failure, "lua_bootstrap", "shared_entries_read", 0,
                    shared_vtable, 0, shared_entries.size(),
                    GetLastError());
    for (std::size_t index = 0; index != shared_entries.size(); ++index) {
        if (index == 4 || index == 5) {
            const std::uint64_t expected =
                image_base + (index == 4 ? 0x31E4B0 : 0x31E460);
            if (shared_entries[index] != expected ||
                !ExecutableRange(process, expected))
                return Fail(failure, "lua_bootstrap",
                            "shared_direct_entry", index,
                            shared_vtable + index * 8,
                            shared_entries[index], expected);
            continue;
        }
        if (!RangeInModule(shared_entries[index], 1, lua_shared) ||
            !ExecutableRange(process, shared_entries[index]))
            return Fail(failure, "lua_bootstrap", "shared_entry", index,
                        shared_vtable + index * 8, shared_entries[index],
                        lua_shared.base);
    }
    std::uint64_t create_original = 0;
    std::uint64_t close_original = 0;
    if (!ReadValue(process, image_base + 0x80AA88, create_original) ||
        !ReadValue(process, image_base + 0x80A9F8, close_original) ||
        !RangeInModule(create_original, 1, lua_shared) ||
        !RangeInModule(close_original, 1, lua_shared) ||
        !ExecutableRange(process, create_original) ||
        !ExecutableRange(process, close_original) ||
        !VerifyBytes(process, create_original, kLuaOriginal13440Prefix,
                     sizeof(kLuaOriginal13440Prefix), failure,
                     "lua_bootstrap", "create_original_prefix", 4) ||
        !VerifyBytes(process, close_original, kLuaOriginal13140Prefix,
                     sizeof(kLuaOriginal13140Prefix), failure,
                     "lua_bootstrap", "close_original_prefix", 5))
        return Fail(failure, "lua_bootstrap", "shared_direct_saved", 0,
                    image_base + 0x80AA88, create_original,
                    lua_shared.base, GetLastError());
    get_lua = shared_entries[6];
    if (!RangeInModule(get_lua, sizeof(kLuaGetInterfacePrefix),
                       lua_shared) ||
        !ExecutableRange(process, get_lua) ||
        !VerifyBytes(process, get_lua, kLuaGetInterfacePrefix,
                     sizeof(kLuaGetInterfacePrefix), failure,
                     "lua_bootstrap", "get_lua_prefix", 0))
        return Fail(failure, "lua_bootstrap", "get_lua", 0, shared,
                    get_lua, lua_shared.base, GetLastError());
    state.get_lua = get_lua;
    if (!RemoteCall3(process, get_lua, shared, 2, 0, state.menu_realm,
                     failure, "lua_get_menu_interface", 2) ||
        !state.menu_realm ||
        !ReadValue(process, state.menu_realm + 8, state.menu_lua_state) ||
        !state.menu_lua_state)
        return Fail(failure, "lua_bootstrap", "menu_realm_present", 2,
                    state.menu_realm,
                    state.menu_lua_state, 1, GetLastError());
    if (!ReadValue(process, state.menu_realm, state.menu_vtable) ||
        !state.menu_vtable ||
        !ResolveLuaRealmVtables(process, lua_shared, state.original,
                                image_base + 0x31DE30,
                                state.menu_vtable, state, failure))
        return false;
    const std::uint64_t realm_slot = image_base + 0x7EA870;
    const std::uint64_t saved_slot = image_base + 0x823868;
    if (!ReadValue(process, realm_slot, state.previous_realm_slot) ||
        !ReadValue(process, saved_slot, state.previous_saved))
        return Fail(failure, "lua_bootstrap", "globals_read", 0,
                    realm_slot, 0, 0, GetLastError());
    std::uint64_t queried = 0;
    if (!RemoteCall3(process, get_lua, shared, 0, 0, queried, failure,
                     "lua_get_interface", 0))
        return false;
    std::uint64_t stored = 0;
    if (!ReadValue(process, realm_slot, stored))
        return Fail(failure, "lua_bootstrap", "realm_slot_read", 0,
                    realm_slot, 0, 0, GetLastError());
    if (queried && stored && queried != stored)
        return Fail(failure, "lua_bootstrap", "realm_identity", 0,
                    realm_slot, stored, queried);
    state.realm = queried ? queried : stored;
    if (!state.realm) {
        if (state.previous_realm_slot || state.previous_saved)
            return Fail(failure, "lua_bootstrap", "deferred_slots_zero", 0,
                        realm_slot, state.previous_realm_slot,
                        state.previous_saved);
        state.deferred = true;
        return true;
    }
    if (state.previous_realm_slot &&
        state.previous_realm_slot != state.realm)
        return Fail(failure, "lua_bootstrap", "stored_realm_exact", 0,
                    realm_slot, state.previous_realm_slot, state.realm);
    std::uint64_t live = 0;
    if (!ReadValue(process, state.realm, state.vtable) || !state.vtable) {
        return Fail(failure, "lua_bootstrap", "realm_vtable", 0,
                    state.realm, state.vtable, 1, GetLastError());
    }
    if (!LuaRealmVtableAccepted(state, state.vtable))
        return Fail(failure, "lua_bootstrap", "realm_vtable_exact", 0,
                    state.realm, state.vtable, state.menu_vtable);
    std::array<std::uint64_t, 132> realm_entries{};
    if (!ReadExact(process, state.vtable, realm_entries.data(),
                   sizeof(realm_entries)))
        return Fail(failure, "lua_bootstrap", "realm_entries_read", 0,
                    state.vtable, 0, realm_entries.size(), GetLastError());
    for (std::size_t index = 0; index != realm_entries.size(); ++index) {
        const bool accepted_detour =
            index == 111 && realm_entries[index] == image_base + 0x31DE30;
        if (!accepted_detour &&
            !RangeInModule(realm_entries[index], 1, lua_shared))
            return Fail(failure, "lua_bootstrap", "realm_entry", index,
                        state.vtable + index * 8, realm_entries[index],
                        lua_shared.base);
        if (!accepted_detour &&
            !ExecutableRange(process, realm_entries[index]))
            return Fail(failure, "lua_bootstrap",
                        "realm_entry_executable", index,
                        state.vtable + index * 8, realm_entries[index], 1);
    }
    state.entry_slot = state.vtable + 111 * 8;
    live = realm_entries[111];
    if (!live)
        return Fail(failure, "lua_bootstrap", "entry_read", 0,
                    state.entry_slot, 0, 0, GetLastError());
    const std::uint64_t detour = image_base + 0x31DE30;
    if (!ExecutableRange(process, detour, kDetourPrefixSize) ||
        !VerifyBytes(process, detour, kDetour31de30, kDetourPrefixSize,
                     failure, "lua_bootstrap", "detour_prefix", 0))
        return false;
    const std::uint64_t expected_original = state.original;
    if (live == detour && state.previous_saved == expected_original &&
        ExecutableRange(process, state.previous_saved) &&
        VerifyBytes(process, state.previous_saved,
                    kLuaRunStringOriginalPrefix,
                    sizeof(kLuaRunStringOriginalPrefix), failure,
                    "lua_bootstrap", "original_prefix", 0)) {
        state.installed = true;
        state.original = state.previous_saved;
    } else if (live == expected_original && state.previous_saved == 0 &&
               ExecutableRange(process, live) &&
               VerifyBytes(process, live, kLuaRunStringOriginalPrefix,
                           sizeof(kLuaRunStringOriginalPrefix), failure,
                           "lua_bootstrap", "original_prefix", 0)) {
        state.original = live;
    } else {
        return Fail(failure, "lua_bootstrap", "entry_saved_consistent", 0,
                    state.entry_slot, live, detour);
    }
    if (!state.previous_realm_slot) {
        if (!WriteProtectedQword(process, realm_slot, state.realm))
            return Fail(failure, "lua_bootstrap", "realm_write", 0,
                        realm_slot, 0, state.realm, GetLastError());
        state.realm_changed = true;
    }
    if (!state.installed) {
        if (!WriteProtectedQword(process, saved_slot, state.original))
            return Fail(failure, "lua_bootstrap", "saved_write", 0,
                        saved_slot, 0, state.original, GetLastError());
        state.saved_changed = true;
        if (!WriteProtectedQword(process, state.entry_slot, detour))
            return Fail(failure, "lua_bootstrap", "entry_write", 0,
                        state.entry_slot, 0, detour, GetLastError());
        state.entry_changed = true;
    }
    return true;
}

bool InstallLuaLinkerReplay(HANDLE process, std::uint64_t image_base,
                            LuaBootstrapState& state, Failure& failure) {
    constexpr std::size_t allocation_size = 0x2000;
    constexpr std::size_t thunk_offset = 0x1000;
    constexpr std::uint64_t sentinel = 0x1122334455667788ull;
    constexpr std::array<std::uint8_t, 8> build_prefix{{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x81, 0xEC}};
    constexpr std::array<std::uint8_t, 8> type_prefix{{
        0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24}};
    constexpr std::array<std::uint8_t, 8> pop_prefix{{
        0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24}};
    constexpr std::array<std::uint8_t, 8> registrar_prefix{{
        0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C}};
    constexpr std::array<std::uint8_t, 8> lookup_prefix{{
        0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x48}};
    constexpr std::array<std::uint8_t, 8> insert_prefix{{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}};

    const std::uint64_t expected_original = state.original;
    const std::uint64_t detour = image_base + 0x31DE30;
    const std::uint64_t saved_slot = image_base + 0x823868;
    if (!expected_original || !ExecutableRange(process, expected_original) ||
        !ExecutableRange(process, detour) ||
        !VerifyBytes(process, expected_original,
                     kLuaRunStringOriginalPrefix,
                     sizeof(kLuaRunStringOriginalPrefix), failure,
                     "lua_linker_replay", "original_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x5A0A0, build_prefix.data(),
                     build_prefix.size(), failure, "lua_linker_replay",
                     "builder_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x55B50, type_prefix.data(),
                     type_prefix.size(), failure, "lua_linker_replay",
                     "type_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x55E80, pop_prefix.data(),
                     pop_prefix.size(), failure, "lua_linker_replay",
                     "pop_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x9C940,
                     registrar_prefix.data(), registrar_prefix.size(),
                     failure, "lua_linker_replay", "registrar_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x618D0, lookup_prefix.data(),
                     lookup_prefix.size(), failure, "lua_linker_replay",
                     "lookup_prefix", 0) ||
         !VerifyBytes(process, image_base + 0x608E0, insert_prefix.data(),
                      insert_prefix.size(), failure, "lua_linker_replay",
                      "insert_prefix", 0))
        return false;

    if (state.deferred) {
        std::uint64_t realm = 0;
        std::uint64_t saved = 0;
        if (!ReadValue(process, image_base + 0x7EA870, realm) ||
            !ReadValue(process, saved_slot, saved) || realm != 0 ||
            saved != 0)
            return Fail(failure, "lua_linker_replay",
                        "deferred_globals_clean", 0,
                        image_base + 0x7EA870, realm, saved,
                        GetLastError());
        for (std::size_t index = 0;
             index != state.realm_vtable_count; ++index) {
            const std::uint64_t slot = state.realm_vtables[index] +
                111 * sizeof(std::uint64_t);
            std::uint64_t live = 0;
            if (!ReadValue(process, slot, live) ||
                live != expected_original)
                return Fail(failure, "lua_linker_replay",
                            "deferred_vtable_clean", index, slot, live,
                            expected_original, GetLastError());
        }

        return true;
    }

    LuaLinkerReplayContext context{};
    context.realm_slot = image_base + 0x7EA870;
    context.original = expected_original;
    context.build_environment = image_base + 0x5A0A0;
    context.is_type = image_base + 0x55B50;
    context.pop = image_base + 0x55E80;
    context.registrar = image_base + 0x9C940;
    context.ownership_global = image_base + 0x7CE8C0;
    context.ownership_lookup = image_base + 0x618D0;
    context.ownership_insert = image_base + 0x608E0;
    context.saved_slot = saved_slot;
    if (!ReadValue(process, image_base + 0x7EA140, context.gettop) ||
        !ReadValue(process, image_base + 0x7EA148, context.settop) ||
        !ReadValue(process, image_base + 0x7EA120, context.getfield) ||
        !ReadValue(process, image_base + 0x7EA158, context.setfield) ||
        !ReadValue(process, image_base + 0x7EA4F8, context.topointer) ||
        !ReadValue(process, image_base + 0x7EA198,
                   context.registrar_context))
        return Fail(failure, "lua_linker_replay", "imports_read", 0,
                    image_base + 0x7EA120, 0, 1, GetLastError());
    const std::array<std::uint64_t, 5> lua_imports{{
        context.gettop, context.settop, context.getfield,
        context.setfield, context.topointer}};
    for (std::size_t index = 0; index != lua_imports.size(); ++index) {
        if (!lua_imports[index] ||
            !ExecutableRange(process, lua_imports[index]))
            return Fail(failure, "lua_linker_replay",
                        "import_executable", index,
                        image_base + 0x7EA120, lua_imports[index], 1,
                        GetLastError());
    }

    auto code = kLuaLinkerReplayThunk;
    std::size_t sentinel_offset = 0;
    std::size_t sentinel_count = 0;
    for (std::size_t index = 0; index + sizeof(sentinel) <= code.size();
         ++index) {
        std::uint64_t candidate = 0;
        std::memcpy(&candidate, code.data() + index, sizeof(candidate));
        if (candidate == sentinel) {
            sentinel_offset = index;
            ++sentinel_count;
        }
    }
    if (sentinel_count != 1)
        return Fail(failure, "lua_linker_replay", "thunk_sentinel", 0,
                    sentinel_offset, sentinel_count, 1);

    void* allocation = VirtualAllocEx(
        process, nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!allocation)
        return Fail(failure, "lua_linker_replay", "allocate", 0, 0, 0,
                    allocation_size, GetLastError());
    const std::uint64_t allocation_address =
        reinterpret_cast<std::uint64_t>(allocation);
    const std::uint64_t thunk = allocation_address + thunk_offset;
    std::memcpy(code.data() + sentinel_offset, &allocation_address,
                sizeof(allocation_address));
    context.saved_slot = saved_slot;

    auto release_unpublished = [&]() {
        VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
    };
    if (!WriteExact(process, allocation_address, &context,
                    sizeof(context)) ||
        !WriteExact(process, thunk, code.data(), code.size())) {
        const DWORD error = GetLastError();
        release_unpublished();
        return Fail(failure, "lua_linker_replay", "write", 0,
                    allocation_address, 0, code.size(), error);
    }
    DWORD old_protection = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(thunk),
                          0x1000, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(process, reinterpret_cast<void*>(thunk),
                               code.size())) {
        const DWORD error = GetLastError();
        release_unpublished();
        return Fail(failure, "lua_linker_replay", "protect", 0, thunk,
                    old_protection, PAGE_READWRITE, error);
    }
    LuaLinkerReplayContext observed_context{};
    std::array<std::uint8_t, kLuaLinkerReplayThunk.size()> observed_code{};
    if (!ReadExact(process, allocation_address, &observed_context,
                   sizeof(observed_context)) ||
        std::memcmp(&observed_context, &context, sizeof(context)) != 0 ||
        !ReadExact(process, thunk, observed_code.data(),
                   observed_code.size()) ||
        observed_code != code || !ExecutableRange(process, thunk, code.size())) {
        const DWORD error = GetLastError();
        release_unpublished();
        return Fail(failure, "lua_linker_replay", "remote_exact", 0,
                    thunk, PrefixValue(observed_code.data(), 8),
                    PrefixValue(code.data(), 8), error);
    }

    state.linker_replay_allocation = allocation_address;
    state.linker_replay_context = allocation_address;
    state.linker_replay_thunk = thunk;
    std::uint64_t saved = 0;
    if (!ReadValue(process, saved_slot, saved) ||
        saved != expected_original) {
        release_unpublished();
        state.linker_replay_allocation = 0;
        state.linker_replay_context = 0;
        state.linker_replay_thunk = 0;
        return Fail(failure, "lua_linker_replay", "saved_prestate", 0,
                    saved_slot, saved, expected_original,
                    GetLastError());
    }
    state.linker_replay_previous_saved = saved;
    const bool saved_write = WriteProtectedQword(process, saved_slot, thunk);
    std::uint64_t saved_after = 0;
    const bool saved_published = ReadValue(process, saved_slot, saved_after) &&
                                 saved_after == thunk;
    if (saved_published) {
        state.linker_replay_published = true;
        state.linker_replay_saved_changed = true;
    }
    if (!saved_write || !saved_published)
        return Fail(failure, "lua_linker_replay", "saved_publish", 0,
                    saved_slot, saved_after, thunk, GetLastError());

    std::uint64_t live = 0;
    if (!ReadValue(process, state.entry_slot, live) || live != detour)
        return Fail(failure, "lua_linker_replay",
                    "entry_poststate", 0, state.entry_slot, live,
                    detour, GetLastError());
    return true;
}

struct ConfigBindingsState {
    std::uint64_t begin_before = 0;
    std::uint64_t end_before = 0;
    std::uint64_t capacity_before = 0;
    std::uint64_t file_find_before = 0;
    std::uint64_t render_find_before = 0;
    std::size_t count_before = 0;
    bool preexisting = false;
    bool initialization_attempted = false;
    bool mutation_possible = false;
    bool preserve_on_rollback = false;
};

struct ConfigBindingSpec {
    const char* table;
    const char* member;
    std::uint32_t callback_rva;
    std::uint32_t owner_rva;
    std::uint32_t registrar_rva;
};

constexpr std::size_t kConfigBindingBaseMaximum = 4;
constexpr std::size_t kConfigBindingCount = 44;
constexpr std::size_t kConfigBindingRecordSize = 0x50;

constexpr std::array<ConfigBindingSpec, kConfigBindingCount>
    kConfigBindingSpecs{{
        {"file", "Exists", 0x16AC50, 0x7EA748, 0x942C0},
        {"file", "Open", 0x16B990, 0x7EA758, 0x942C0},
        {"file", "AsyncRead", 0x16C830, 0x7EA760, 0x942C0},
        {"file", "CreateDir", 0x16D480, 0x7EA7A8, 0x942C0},
        {"file", "Delete", 0x16DE10, 0x7EA768, 0x942C0},
        {"file", "IsDir", 0x16E7B0, 0x7EA7B0, 0x942C0},
        {"file", "Rename", 0x16F390, 0x7EA7C0, 0x942C0},
        {"file", "Size", 0x16FF10, 0x7EA7B8, 0x942C0},
        {"file", "Time", 0x170B80, 0x7EA750, 0x942C0},
        {"file", "Find", 0x1717F0, 0x7EA770, 0x942C0},
        {"net", "Start", 0x172430, 0x7EA740, 0x942C0},
        {"net", "SendToServer", 0x1783C0, 0x7EA710, 0x942C0},
        {"net", "Abort", 0x177E10, 0x7EA688, 0x942C0},
        {"net", "WriteBit", 0x172CA0, 0x7EA6C0, 0x942C0},
        {"net", "WriteUInt", 0x1737D0, 0x7EA6B0, 0x942C0},
        {"net", "WriteInt", 0x173DB0, 0x7EA708, 0x942C0},
        {"net", "WriteString", 0x174360, 0x7EA6A0, 0x942C0},
        {"net", "WriteFloat", 0x174BF0, 0x7EA6F0, 0x942C0},
        {"net", "WriteDouble", 0x175200, 0x7EA728, 0x942C0},
        {"net", "WriteVector", 0x175820, 0x7EA730, 0x942C0},
        {"net", "WriteAngle", 0x1760F0, 0x7EA6A8, 0x942C0},
        {"net", "WriteMatrix", 0x1769A0, 0x7EA698, 0x942C0},
        {"net", "WriteData", 0x1773F0, 0x7EA6D0, 0x942C0},
        {"net", "ReadHeader", 0x1793E0, 0x7EA700, 0x942C0},
        {"net", "ReadBit", 0x17A1E0, 0x7EA738, 0x942C0},
        {"net", "ReadUInt", 0x17A7B0, 0x7EA690, 0x942C0},
        {"net", "ReadInt", 0x17AE30, 0x7EA6E0, 0x942C0},
        {"net", "ReadString", 0x17B4A0, 0x7EA6C8, 0x942C0},
        {"net", "ReadFloat", 0x17BC10, 0x7EA6F8, 0x942C0},
        {"net", "ReadDouble", 0x17C1C0, 0x7EA6E8, 0x942C0},
        {"net", "ReadVector", 0x17C770, 0x7EA6B8, 0x942C0},
        {"net", "ReadAngle", 0x17CB90, 0x7EA668, 0x942C0},
        {"net", "ReadMatrix", 0x17CFB0, 0x7EA718, 0x942C0},
        {"net", "ReadData", 0x17D530, 0x7EA6D8, 0x942C0},
        {"engine", "WriteDupe", 0x180FA0, 0x7EA7C8, 0x942C0},
        {"engine", "OpenDupe", 0x181A40, 0x7EA788, 0x942C0},
        {"steamworks", "DownloadUGC", 0x1825C0, 0x7EA678, 0x942C0},
        {"game", "MountGMA", 0x182720, 0x7EA7A0, 0x942C0},
        {"GLOBAL", "HTTP", 0x182850, 0x7EA680, 0x942C0},
        {"sql", "Query", 0x1847B0, 0x7EA720, 0x942C0},
        {"GLOBAL", "RunConsoleCommand", 0x1850F0, 0x7EA790, 0x942C0},
        {"Player", "ConCommand", 0x1869C0, 0x7EA670, 0x94520},
        {"GLOBAL", "AddConsoleCommand", 0x185EF0, 0x7EA798, 0x942C0},
        {"GLOBAL", "CreateConVar", 0x187400, 0x7EA780, 0x942C0},
    }};

struct ConfigBindingString {
    std::array<char, 16> storage{};
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
};

struct ConfigBindingRecord {
    std::uint64_t owner = 0;
    std::uint64_t callback = 0;
    ConfigBindingString member{};
    ConfigBindingString table{};
};

static_assert(sizeof(ConfigBindingString) == 0x20);
static_assert(sizeof(ConfigBindingRecord) == kConfigBindingRecordSize);

bool ConfigBindingStringMatches(HANDLE process,
                                const ConfigBindingString& value,
                                const char* expected) {
    const std::size_t expected_size = std::strlen(expected);
    if (expected_size >= 64 || value.size != expected_size ||
        value.capacity < value.size || value.capacity > 4096)
        return false;
    std::array<char, 64> observed{};
    if (value.capacity <= 15) {
        if (value.capacity != 15)
            return false;
        std::memcpy(observed.data(), value.storage.data(),
                    expected_size + 1);
    } else {
        std::uint64_t text = 0;
        std::memcpy(&text, value.storage.data(), sizeof(text));
        if (!text ||
            !ReadExact(process, text, observed.data(), expected_size + 1))
            return false;
    }
    return observed[expected_size] == 0 &&
           std::memcmp(observed.data(), expected, expected_size) == 0;
}

bool ConfigBindingRecordMatches(HANDLE process,
                                const ConfigBindingRecord& record,
                                const ConfigBindingSpec& spec,
                                std::uint64_t image_base) {
    std::uint64_t trampoline = 0;
    return record.owner == image_base + spec.owner_rva &&
           ReadValue(process, record.owner, trampoline) && trampoline &&
           ExecutableRange(process, trampoline, 5) &&
           record.callback == image_base + spec.callback_rva &&
           ExecutableRange(process, record.callback, 5) &&
           ConfigBindingStringMatches(process, record.table, spec.table) &&
           ConfigBindingStringMatches(process, record.member, spec.member);
}

bool FindConfigBindingProgress(
    HANDLE process, std::uint64_t image_base,
    const std::array<ConfigBindingRecord,
                     kConfigBindingBaseMaximum + kConfigBindingCount>& records,
    std::size_t count, std::size_t& baseline, std::size_t& progress) {
    std::size_t matches = 0;
    baseline = 0;
    progress = 0;
    const std::size_t maximum_baseline =
        std::min(count, kConfigBindingBaseMaximum);
    for (std::size_t candidate = 0; candidate <= maximum_baseline;
         ++candidate) {
        const std::size_t candidate_progress = count - candidate;
        if (!candidate_progress || candidate_progress > kConfigBindingCount)
            continue;
        bool exact = true;
        for (std::size_t index = 0; index != candidate_progress; ++index) {
            if (!ConfigBindingRecordMatches(
                    process, records[candidate + index],
                    kConfigBindingSpecs[index], image_base)) {
                exact = false;
                break;
            }
        }
        if (!exact)
            continue;
        ++matches;
        baseline = candidate;
        progress = candidate_progress;
    }
    return matches == 1;
}

struct ConfigStringPairBlock {
    ConfigBindingString table{};
    ConfigBindingString member{};
    std::array<char, 32> table_source{};
    std::array<char, 64> member_source{};
};

bool InvokeConfigStringPair(HANDLE process, std::uint64_t image_base,
                            const char* table, const char* member,
                            std::uint64_t function, std::uint64_t argument3,
                            std::uint64_t argument4, bool four_arguments,
                            std::uint64_t& result, Failure& failure,
                            const char* stage, std::size_t index) {
    ConfigStringPairBlock block{};
    const std::size_t table_size = std::strlen(table) + 1;
    const std::size_t member_size = std::strlen(member) + 1;
    if (table_size > block.table_source.size() ||
        member_size > block.member_source.size())
        return Fail(failure, stage, "name_bounded", index, table_size,
                    member_size, block.member_source.size());
    std::memcpy(block.table_source.data(), table, table_size);
    std::memcpy(block.member_source.data(), member, member_size);
    void* remote = VirtualAllocEx(process, nullptr, sizeof(block),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return Fail(failure, stage, "argument_allocate", index, 0, 0,
                    sizeof(block), GetLastError());
    const auto address = reinterpret_cast<std::uint64_t>(remote);
    const auto table_object =
        address + offsetof(ConfigStringPairBlock, table);
    const auto member_object =
        address + offsetof(ConfigStringPairBlock, member);
    const auto table_source =
        address + offsetof(ConfigStringPairBlock, table_source);
    const auto member_source =
        address + offsetof(ConfigStringPairBlock, member_source);
    if (!WriteExact(process, address, &block, sizeof(block))) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return Fail(failure, stage, "argument_write", index, address, 0,
                    sizeof(block), error);
    }

    bool table_live = false;
    bool member_live = false;
    auto destroy_live = [&]() {
        if (!RemoteCallsRollbackSafe())
            return false;
        Failure ignored{};
        std::uint64_t ignored_result = 0;
        bool ok = true;
        if (member_live) {
            bool completed = false;
            ok = RemoteCall3(process, image_base + 0x10CE0,
                             member_object, 0, 0, ignored_result, ignored,
                             stage, index, &completed) && completed && ok;
            if (completed)
                member_live = false;
        }
        if (table_live) {
            bool completed = false;
            ok = RemoteCall3(process, image_base + 0x10CE0,
                             table_object, 0, 0, ignored_result, ignored,
                             stage, index, &completed) && completed && ok;
            if (completed)
                table_live = false;
        }
        return ok;
    };

    std::uint64_t constructor_result = 0;
    bool table_constructed = false;
    const bool table_call = RemoteCall3(
        process, image_base + 0xCA20, table_object, table_source, 0,
        constructor_result, failure, stage, index, &table_constructed);
    table_live = table_constructed;
    if (!table_call) {
        destroy_live();
        if (RemoteCallsRollbackSafe())
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    bool member_constructed = false;
    const bool member_call = RemoteCall3(
        process, image_base + 0xCA20, member_object, member_source, 0,
        constructor_result, failure, stage, index, &member_constructed);
    member_live = member_constructed;
    if (!member_call) {
        destroy_live();
        if (RemoteCallsRollbackSafe())
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    bool target_completed = false;
    const bool called = four_arguments
        ? RemoteCall4(process, function, table_object, member_object,
                      argument3, argument4, result, failure, stage, index,
                      &target_completed)
        : RemoteCall3(process, function, table_object, member_object,
                      argument3, result, failure, stage, index,
                      &target_completed);
    if (target_completed) {
        table_live = false;
        member_live = false;
    }
    if (!called) {
        destroy_live();
        if (RemoteCallsRollbackSafe() && !table_live && !member_live)
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    if (table_live || member_live ||
        !VirtualFreeEx(process, remote, 0, MEM_RELEASE))
        return Fail(failure, stage, "argument_free", index, address,
                    table_live, member_live, GetLastError());
    return true;
}

bool CallConfigBindingRegistrar(HANDLE process, std::uint64_t image_base,
                                const ConfigBindingSpec& spec,
                                Failure& failure, std::size_t index) {
    std::uint64_t owner = UINT64_MAX;
    if (!ReadValue(process, image_base + spec.owner_rva, owner) || owner != 0)
        return Fail(failure, "config_binding_resume", "owner_pristine",
                    index, image_base + spec.owner_rva, owner, 0,
                    GetLastError());
    std::uint64_t result = 0;
    if (!InvokeConfigStringPair(
            process, image_base, spec.table, spec.member,
            image_base + spec.registrar_rva,
            image_base + spec.callback_rva, image_base + spec.owner_rva,
            true, result, failure, "config_binding_resume", index))
        return false;
    if ((result & 0xFFu) != 1)
        return Fail(failure, "config_binding_resume", "register_result",
                    index, image_base + spec.registrar_rva, result, 1);
    if (!ReadValue(process, image_base + spec.owner_rva, owner) || !owner ||
        !ExecutableRange(process, owner, 5))
        return Fail(failure, "config_binding_resume", "owner_executable",
                    index, image_base + spec.owner_rva, owner, 1,
                    GetLastError());
    return true;
}

bool LookupConfigFunction(HANDLE process, std::uint64_t image_base,
                          const char* table, const char* member,
                          std::uint64_t& result, Failure& failure,
                          std::size_t index) {
    result = 0;
    if (!InvokeConfigStringPair(
            process, image_base, table, member, image_base + 0x93C00, 0,
            0, false, result, failure, "config_binding_lookup", index))
        return false;
    if (!result || !ExecutableRange(process, result, 5))
        return Fail(failure, "config_binding_lookup", "result_executable",
                    index, result, 0, 1, GetLastError());
    return true;
}

bool EnsureConfigFunctionPointers(HANDLE process, std::uint64_t image_base,
                                  Failure& failure) {
    std::uint64_t network_string_to_id = 0;
    std::uint64_t network_id_to_string = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_left = 0;
    if (!LookupConfigFunction(process, image_base, "util",
                              "NetworkStringToID", network_string_to_id,
                              failure, 0) ||
        !LookupConfigFunction(process, image_base, "util",
                              "NetworkIDToString", network_id_to_string,
                              failure, 1) ||
        !LookupConfigFunction(process, image_base, "net", "BytesWritten",
                              bytes_written, failure, 2) ||
        !LookupConfigFunction(process, image_base, "net", "BytesLeft",
                              bytes_left, failure, 3))
        return false;
    if (!WriteProtectedQword(process, image_base + 0xB6B540,
                             network_id_to_string) ||
        !WriteProtectedQword(process, image_base + 0xB6B548, bytes_left))
        return Fail(failure, "config_binding_lookup", "publish", 0,
                    image_base + 0xB6B540, network_id_to_string,
                    bytes_left, GetLastError());
    std::uint64_t observed_file = 0;
    std::uint64_t observed_render = 0;
    if (!ReadValue(process, image_base + 0xB6B540, observed_file) ||
        !ReadValue(process, image_base + 0xB6B548, observed_render) ||
        observed_file != network_id_to_string || observed_render != bytes_left)
        return Fail(failure, "config_binding_lookup", "publish_exact", 0,
                    image_base + 0xB6B540, observed_file,
                    network_id_to_string, GetLastError());
    return true;
}

bool EnsureConfigBindings(HANDLE process, std::uint64_t image_base,
                          ConfigBindingsState& state, Failure& failure) {
    constexpr std::uint32_t vector_rva = 0xB6B3F8;
    constexpr std::size_t maximum_count =
        kConfigBindingBaseMaximum + kConfigBindingCount;
    constexpr std::array<std::uint8_t, 16> initializer_prefix{{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C,
        0x24, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83}};
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    std::uint64_t file_find = 0;
    std::uint64_t render_find = 0;
    auto read_state = [&]() {
        return ReadValue(process, image_base + vector_rva, begin) &&
               ReadValue(process, image_base + vector_rva + 8, end) &&
               ReadValue(process, image_base + vector_rva + 16,
                         capacity) &&
               ReadValue(process, image_base + 0xB6B540, file_find) &&
               ReadValue(process, image_base + 0xB6B548, render_find);
    };
    auto valid_vector = [&]() {
        if (!begin && !end && !capacity)
            return true;
        if (!begin || end < begin || capacity < end ||
            (end - begin) % kConfigBindingRecordSize != 0 ||
            (capacity - begin) % kConfigBindingRecordSize != 0)
            return false;
        const std::uint64_t used = end - begin;
        return used <= maximum_count * kConfigBindingRecordSize &&
               (!used || ReadableRange(process, begin,
                                        static_cast<std::size_t>(used)));
    };
    std::array<ConfigBindingRecord, maximum_count> records{};
    auto read_records = [&](std::size_t count) {
        records = {};
        return !count || ReadExact(
            process, begin, records.data(),
            count * sizeof(ConfigBindingRecord));
    };
    auto read_and_classify = [&](std::size_t& count,
                                 std::size_t& baseline,
                                 std::size_t& progress) {
        if (!read_state() || !valid_vector())
            return false;
        count = begin
            ? static_cast<std::size_t>((end - begin) /
                                       kConfigBindingRecordSize)
            : 0;
        return read_records(count) &&
               FindConfigBindingProgress(process, image_base, records,
                                         count, baseline, progress);
    };
    if (!read_state())
        return Fail(failure, "config_bindings", "state_read", 0,
                    image_base + vector_rva, 0, 0, GetLastError());
    if (!valid_vector())
        return Fail(failure, "config_bindings", "vector_shape", 0,
                    image_base + vector_rva, end, begin);
    std::size_t count = begin
                            ? static_cast<std::size_t>((end - begin) /
                                                       kConfigBindingRecordSize)
                             : 0;
    state.begin_before = begin;
    state.end_before = end;
    state.capacity_before = capacity;
    state.file_find_before = file_find;
    state.render_find_before = render_find;
    state.count_before = count;

    if (!read_records(count))
        return Fail(failure, "config_bindings", "records_read", 0, begin,
                    0, count * sizeof(ConfigBindingRecord), GetLastError());
    std::size_t baseline = 0;
    std::size_t progress = 0;
    const bool recognized = FindConfigBindingProgress(
        process, image_base, records, count, baseline, progress);
    if (recognized && progress == kConfigBindingCount && file_find &&
        render_find && ExecutableRange(process, file_find, 5) &&
        ExecutableRange(process, render_find, 5)) {
        state.preexisting = true;
        return true;
    }

    auto verify_progress = [&](std::size_t expected_progress,
                               const char* predicate) {
        std::size_t observed_count = 0;
        std::size_t observed_baseline = 0;
        std::size_t observed_progress = 0;
        if (!read_and_classify(observed_count, observed_baseline,
                               observed_progress) ||
            observed_baseline != baseline ||
            observed_progress != expected_progress ||
            observed_count != baseline + expected_progress)
            return Fail(failure, "config_bindings", predicate,
                        expected_progress, image_base + vector_rva,
                        observed_count, baseline + expected_progress,
                        GetLastError());
        return true;
    };

    if (recognized && progress < kConfigBindingCount) {
        if (progress < 10 && (file_find || render_find))
            return Fail(failure, "config_bindings",
                        "partial_pointer_order", progress,
                        image_base + 0xB6B540, file_find, 0);
        state.initialization_attempted = true;
        state.mutation_possible = true;
        state.preserve_on_rollback = true;

        for (std::size_t index = progress; index < 10; ++index) {
            if (!CallConfigBindingRegistrar(process, image_base,
                                            kConfigBindingSpecs[index],
                                            failure, index) ||
                !verify_progress(index + 1, "file_resume_increment"))
                return false;
        }
        if (!EnsureConfigFunctionPointers(process, image_base, failure))
            return false;
        for (std::size_t index = std::max<std::size_t>(progress, 10);
             index < kConfigBindingCount; ++index) {
            if (!CallConfigBindingRegistrar(process, image_base,
                                            kConfigBindingSpecs[index],
                                            failure, index) ||
                !verify_progress(index + 1, "binding_resume_increment"))
                return false;
        }
        if (!read_state() || !valid_vector() || !file_find || !render_find ||
            !ExecutableRange(process, file_find, 5) ||
            !ExecutableRange(process, render_find, 5) ||
            !verify_progress(kConfigBindingCount, "resume_exact_poststate"))
            return Fail(failure, "config_bindings", "resume_poststate", 0,
                        image_base + vector_rva, count,
                        baseline + kConfigBindingCount, GetLastError());
        return true;
    }

    if (count > kConfigBindingBaseMaximum || file_find || render_find)
        return Fail(failure, "config_bindings", "ordered_prestate", 0,
                    image_base + vector_rva, count,
                    kConfigBindingBaseMaximum);
    const std::uint64_t initializer = image_base + 0x188090;
    if (!ExecutableRange(process, initializer, initializer_prefix.size()) ||
        !VerifyBytes(process, initializer, initializer_prefix.data(),
                     initializer_prefix.size(), failure,
                     "config_bindings", "initializer_prefix", 0))
        return false;
    std::uint64_t result = UINT64_MAX;
    state.initialization_attempted = true;
    state.mutation_possible = true;
    state.preserve_on_rollback = true;
    const bool called =
        RemoteCall3(process, initializer, 0, 0, 0, result, failure,
                    "config_bindings_call", 0);
    if (!RemoteCallsRollbackSafe())
        return false;
    if (!read_state()) {
        if (!called)
            return false;
        return Fail(failure, "config_bindings", "post_read", 0,
                    image_base + vector_rva, 0, 0, GetLastError());
    }
    if (!called)
        return false;
    if (!valid_vector())
        return Fail(failure, "config_bindings", "post_vector_shape", 0,
                    image_base + vector_rva, end, begin);
    count = begin ? static_cast<std::size_t>((end - begin) /
                                             kConfigBindingRecordSize)
                  : 0;
    records = {};
    std::size_t final_baseline = 0;
    std::size_t final_progress = 0;
    if (!read_records(count) ||
        !FindConfigBindingProgress(process, image_base, records, count,
                                   final_baseline, final_progress) ||
        final_baseline != state.count_before ||
        final_progress != kConfigBindingCount ||
        !file_find || !render_find ||
        !ExecutableRange(process, file_find, 5) ||
        !ExecutableRange(process, render_find, 5))
        return Fail(failure, "config_bindings", "exact_poststate", 0,
                    image_base + vector_rva, count,
                    state.count_before + kConfigBindingCount,
                    GetLastError());
    return true;
}

struct ConfigRegistryEntry {
    std::uint64_t node = 0;
    std::string key;
    std::array<std::uint8_t, 4> value{};
};

struct ConfigRegistrySnapshot {
    std::uint64_t sentinel = 0;
    std::uint64_t count = 0;
    std::vector<ConfigRegistryEntry> entries;
};

struct ConfigRegistrySpec {
    std::uint32_t map_rva;
    std::uint32_t sentinel_rva;
    std::uint32_t count_rva;
    std::uint32_t accessor_rva;
    std::size_t value_size;
    const char* const* keys;
    std::size_t key_count;
};

constexpr std::array<const char*, 18> kConfigBoolPrewarmKeys{{
    "entities_index",
    "esp_other_crosshair",
    "esp_other_crosshair_outline",
    "esp_player_distance",
    "esp_player_hardcoded_bbox",
    "esp_player_simulation",
    "esp_player_teambased",
    "esp_player_velocity",
    "exploits_cusercmd_override",
    "exploits_cusercmd_restoration",
    "exploits_fake_latency",
    "misc_block_luacmd",
    "misc_faststop",
    "playerlist_kirkware",
    "rage_antiaim_fake_enable",
    "exploits_rapidfire",
    "exploits_tickbase_shift_fakecommands",
    "exploits_tickbase_set_uncharge_rate",
}};

constexpr std::array<const char*, 69> kConfigIntPrewarmKeys{{
    "chams_backtrack_material_color_options",
    "chams_backtrack_overlay_color_options",
    "chams_hands_material",
    "chams_hands_overlay",
    "chams_selection",
    "chams_weapon_material",
    "chams_weapon_overlay",
    "console_key_style",
    "entities_click_manager_key",
    "entities_click_manager_key_style",
    "entities_box_style",
    "esp_box_style",
    "esp_player_hitsound_type",
    "exploits_breaklc_key",
    "exploits_breaklc_key_style",
    "exploits_desynculator_key",
    "exploits_desynculator_key_style",
    "exploits_fake_latency_key",
    "exploits_fake_latency_key_style",
    "exploits_fakeduck_key",
    "exploits_fakeduck_key_style",
    "exploits_seq_freeze_key",
    "exploits_seq_freeze_key_style",
    "legit_enable_key",
    "legit_enable_key_style",
    "legit_triggerbot_key",
    "legit_triggerbot_key_style",
    "menu_key_style",
    "misc_auto_strafe_back_key_style",
    "misc_auto_strafe_forward_key_style",
    "misc_auto_strafe_key",
    "misc_auto_strafe_key_style",
    "misc_auto_strafe_left_key_style",
    "misc_auto_strafe_right_key_style",
    "misc_bunnyhop_key",
    "misc_bunnyhop_key_style",
    "misc_fakelag_key",
    "misc_fakelag_key_style",
    "misc_freecam_back_key_style",
    "misc_freecam_down_key_style",
    "misc_freecam_forward_key_style",
    "misc_freecam_key",
    "misc_freecam_key_style",
    "misc_freecam_left_key_style",
    "misc_freecam_right_key_style",
    "misc_freecam_spectate_key_style",
    "misc_freecam_speed_key_style",
    "misc_freecam_stop_spectate_key_style",
    "misc_freecam_up_key_style",
    "misc_thirdperson_key",
    "misc_thirdperson_key_style",
    "misc_zoom_key",
    "misc_zoom_key_style",
    "rage_antiaim_direction",
    "rage_antiaim_enable_key",
    "rage_antiaim_enable_key_style",
    "rage_antiaim_pitch_type",
    "rage_antiaim_yaw_type",
    "rage_enable_key",
    "rage_enable_key_style",
    "exploits_rapidfire_key",
    "exploits_rapidfire_key_style",
    "exploits_slowmotion_key",
    "exploits_slowmotion_key_style",
    "exploits_tickbase_shift_key",
    "exploits_tickbase_shift_key_style",
    "exploits_tickbase_shift_key2",
    "exploits_tickbase_shift_key2_style",
    "exploits_tickbase_uncharge_rate",
}};

constexpr std::array<const char*, 5> kConfigFloatPrewarmKeys{{
    "legit_bezier_0",
    "legit_bezier_1",
    "legit_bezier_2",
    "legit_bezier_3",
    "misc_thirdperson_height",
}};

constexpr std::array<const char*, 36> kConfigKeyStatePrewarmKeys{{
    "console_key",
    "entities_click_manager_key",
    "exploits_breaklc_key",
    "exploits_desynculator_key",
    "exploits_fake_latency_key",
    "exploits_fakeduck_key",
    "exploits_freeze_players_key",
    "exploits_rapidfire_key",
    "exploits_seq_freeze_key",
    "exploits_slowmotion_key",
    "exploits_tickbase_shift_key",
    "exploits_tickbase_shift_key2",
    "legit_enable_key",
    "legit_triggerbot_key",
    "menu_key",
    "misc_auto_strafe_back_key",
    "misc_auto_strafe_forward_key",
    "misc_auto_strafe_key",
    "misc_auto_strafe_left_key",
    "misc_auto_strafe_right_key",
    "misc_bunnyhop_key",
    "misc_fakelag_key",
    "misc_freecam_back_key",
    "misc_freecam_down_key",
    "misc_freecam_forward_key",
    "misc_freecam_key",
    "misc_freecam_left_key",
    "misc_freecam_right_key",
    "misc_freecam_spectate_key",
    "misc_freecam_speed_key",
    "misc_freecam_stop_spectate_key",
    "misc_freecam_up_key",
    "misc_thirdperson_key",
    "misc_zoom_key",
    "rage_antiaim_enable_key",
    "rage_enable_key",
}};

const ConfigRegistryEntry* FindConfigRegistryEntry(
    const ConfigRegistrySnapshot& snapshot, const char* key) {
    for (const auto& entry : snapshot.entries) {
        if (entry.key == key)
            return &entry;
    }
    return nullptr;
}

bool ReadConfigRegistrySnapshot(HANDLE process, std::uint64_t image_base,
                                const ConfigRegistrySpec& spec,
                                ConfigRegistrySnapshot& snapshot,
                                Failure& failure, std::size_t index,
                                const char* stage) {
    snapshot = {};
    if (!ReadValue(process, image_base + spec.sentinel_rva,
                   snapshot.sentinel) ||
        !ReadValue(process, image_base + spec.count_rva, snapshot.count))
        return Fail(failure, stage, "header_read", index,
                    image_base + spec.sentinel_rva, 0, 1,
                    GetLastError());
    if (!snapshot.sentinel || snapshot.count > 2048 ||
        !ReadableRange(process, snapshot.sentinel, 16))
        return Fail(failure, stage, "header_shape", index,
                    snapshot.sentinel, snapshot.count, 2048);
    std::uint64_t node = 0;
    std::uint64_t sentinel_previous = 0;
    if (!ReadValue(process, snapshot.sentinel, node) ||
        !ReadValue(process, snapshot.sentinel + 8, sentinel_previous))
        return Fail(failure, stage, "sentinel_read", index,
                    snapshot.sentinel, 0, 1, GetLastError());
    std::uint64_t previous = snapshot.sentinel;
    while (node != snapshot.sentinel) {
        if (!node || snapshot.entries.size() >= snapshot.count ||
            !ReadableRange(process, node, 0x34))
            return Fail(failure, stage, "node_shape", index, node,
                        snapshot.entries.size(), snapshot.count);
        for (const auto& entry : snapshot.entries) {
            if (entry.node == node)
                return Fail(failure, stage, "node_cycle", index, node,
                            snapshot.entries.size(), snapshot.count);
        }
        std::uint64_t next = 0;
        std::uint64_t node_previous = 0;
        std::uint64_t length = 0;
        std::uint64_t capacity = 0;
        if (!ReadValue(process, node, next) ||
            !ReadValue(process, node + 8, node_previous) ||
            !ReadValue(process, node + 0x20, length) ||
            !ReadValue(process, node + 0x28, capacity))
            return Fail(failure, stage, "node_read", index, node, 0, 1,
                        GetLastError());
        if (node_previous != previous)
            return Fail(failure, stage, "previous_link", index, node,
                        node_previous, previous);
        if (length > 256 || capacity < length)
            return Fail(failure, stage, "key_shape", index, node,
                        length, capacity);
        std::uint64_t text = node + 0x10;
        if (capacity > 15 &&
            (!ReadValue(process, node + 0x10, text) || !text))
            return Fail(failure, stage, "key_pointer", index, node,
                        text, 1, GetLastError());
        std::string key(static_cast<std::size_t>(length), '\0');
        if (length &&
            !ReadExact(process, text, key.data(),
                       static_cast<std::size_t>(length)))
            return Fail(failure, stage, "key_read", index, text,
                        length, 1, GetLastError());
        if (FindConfigRegistryEntry(snapshot, key.c_str()))
            return Fail(failure, stage, "duplicate_key", index, node,
                        snapshot.entries.size(), snapshot.count);
        ConfigRegistryEntry entry{};
        entry.node = node;
        entry.key = std::move(key);
        if (!ReadExact(process, node + 0x30, entry.value.data(),
                       spec.value_size))
            return Fail(failure, stage, "value_read", index,
                        node + 0x30, 0, spec.value_size, GetLastError());
        snapshot.entries.push_back(std::move(entry));
        previous = node;
        node = next;
    }
    if (snapshot.entries.size() != snapshot.count)
        return Fail(failure, stage, "count_exact", index,
                    image_base + spec.count_rva, snapshot.entries.size(),
                    snapshot.count);
    if (sentinel_previous != previous)
        return Fail(failure, stage, "sentinel_previous", index,
                    snapshot.sentinel + 8, sentinel_previous, previous);
    return true;
}

bool AccessConfigRegistryKey(HANDLE process, std::uint64_t image_base,
                             const ConfigRegistrySpec& spec,
                             const char* key, void* remote_page,
                             Failure& failure, std::size_t index) {
    constexpr std::size_t string_size = 32;
    constexpr std::size_t text_offset = 0x100;
    const std::size_t key_size = std::strlen(key) + 1;
    if (key_size > 0x100)
        return Fail(failure, "config_prewarm", "key_size", index, 0,
                    key_size, 0x100);
    const auto object = reinterpret_cast<std::uint64_t>(remote_page);
    const auto text = object + text_offset;
    std::array<std::uint8_t, string_size> zero{};
    if (!WriteExact(process, object, zero.data(), zero.size()) ||
        !WriteExact(process, text, key, key_size))
        return Fail(failure, "config_prewarm", "argument_write", index,
                    object, 0, 1, GetLastError());
    std::uint64_t result = UINT64_MAX;
    bool constructed = false;
    Failure call_failure{};
    const bool constructor_called = RemoteCall3(
        process, image_base + 0xCA20, object, text, 0, result,
        call_failure, "config_prewarm_ctor", index, &constructed);
    if (!constructor_called) {
        failure = call_failure;
        if (!constructed || !RemoteCallsRollbackSafe())
            return false;
    }
    bool accessed = false;
    result = 0;
    const bool accessor_called = constructor_called && RemoteCall3(
        process, image_base + spec.accessor_rva,
        image_base + spec.map_rva, object, 0, result, call_failure,
        "config_prewarm_access", index, &accessed);
    const std::uint64_t value = result;
    Failure destructor_failure{};
    std::uint64_t destructor_result = UINT64_MAX;
    bool destructed = false;
    const bool destructor_called = RemoteCall3(
        process, image_base + 0x10CE0, object, 0, 0,
        destructor_result, destructor_failure, "config_prewarm_dtor",
        index, &destructed);
    if (!destructor_called || !destructed) {
        failure = destructor_failure;
        return false;
    }
    if (!constructor_called || !accessor_called || !accessed) {
        failure = call_failure;
        return false;
    }
    if (!value || !ReadableRange(process, value, spec.value_size))
        return Fail(failure, "config_prewarm", "value_pointer", index,
                    value, 0, 1);
    return true;
}

bool VerifyConfigRegistryPrewarm(
    const ConfigRegistrySpec& spec,
    const ConfigRegistrySnapshot& before,
    const ConfigRegistrySnapshot& after,
    Failure& failure, std::size_t index) {
    std::size_t missing = 0;
    for (std::size_t key_index = 0; key_index != spec.key_count;
         ++key_index) {
        const char* key = spec.keys[key_index];
        if (!FindConfigRegistryEntry(before, key))
            ++missing;
        if (!FindConfigRegistryEntry(after, key))
            return Fail(failure, "config_prewarm_verify", "key_present",
                        index, key_index, 0, 1);
    }
    if (after.count != before.count + missing)
        return Fail(failure, "config_prewarm_verify", "count_delta",
                    index, after.count, before.count + missing);
    for (const auto& entry : before.entries) {
        const auto* current = FindConfigRegistryEntry(after,
                                                      entry.key.c_str());
        if (!current ||
            !std::equal(entry.value.begin(),
                        entry.value.begin() + spec.value_size,
                        current->value.begin()))
            return Fail(failure, "config_prewarm_verify",
                        "value_preserved", index, entry.node, 0, 1);
    }
    return true;
}

bool EnsureConfigRegistryPrewarm(HANDLE process, std::uint64_t image_base,
                                 Failure& failure) {
    constexpr std::array<std::uint8_t, 16> constructor_prefix{{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0,
        0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x01, 0x48, 0x89}};
    constexpr std::array<std::uint8_t, 16> destructor_prefix{{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
        0x51, 0x18, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xFA}};
    constexpr std::array<std::uint8_t, 16> int_accessor_prefix{{
        0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x48,
        0x8D, 0x54, 0x24, 0x20, 0xE8, 0xBF, 0x0E, 0x00}};
    constexpr std::array<std::uint8_t, 16> bool_accessor_prefix{{
        0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x48,
        0x8D, 0x54, 0x24, 0x20, 0xE8, 0xAF, 0x12, 0x00}};
    constexpr std::array<std::uint8_t, 16> float_accessor_prefix{{
        0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x48,
        0x8D, 0x54, 0x24, 0x20, 0xE8, 0x5F, 0x0B, 0x00}};
    constexpr std::array<std::uint8_t, 16> key_accessor_prefix{{
        0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x48,
        0x8D, 0x54, 0x24, 0x20, 0xE8, 0x8F, 0x10, 0x00}};
    if (!VerifyBytes(process, image_base + 0xCA20,
                     constructor_prefix.data(), constructor_prefix.size(),
                     failure, "config_prewarm", "constructor_prefix", 0) ||
        !VerifyBytes(process, image_base + 0x10CE0,
                     destructor_prefix.data(), destructor_prefix.size(),
                     failure, "config_prewarm", "destructor_prefix", 0) ||
        !VerifyBytes(process, image_base + 0xF280,
                     int_accessor_prefix.data(), int_accessor_prefix.size(),
                     failure, "config_prewarm", "int_accessor_prefix", 0) ||
        !VerifyBytes(process, image_base + 0xF340,
                     bool_accessor_prefix.data(), bool_accessor_prefix.size(),
                     failure, "config_prewarm", "bool_accessor_prefix", 0) ||
        !VerifyBytes(process, image_base + 0xF120,
                     float_accessor_prefix.data(),
                     float_accessor_prefix.size(), failure,
                     "config_prewarm", "float_accessor_prefix", 0) ||
        !VerifyBytes(process, image_base + 0xF320,
                     key_accessor_prefix.data(), key_accessor_prefix.size(),
                     failure, "config_prewarm", "key_accessor_prefix", 0))
        return false;
    const std::array<ConfigRegistrySpec, 4> specs{{
        {0xB6E200, 0xB6E208, 0xB6E210, 0xF340, 1,
         kConfigBoolPrewarmKeys.data(), kConfigBoolPrewarmKeys.size()},
        {0xB6CA70, 0xB6CA78, 0xB6CA80, 0xF280, 4,
         kConfigIntPrewarmKeys.data(), kConfigIntPrewarmKeys.size()},
        {0xB6CAF0, 0xB6CAF8, 0xB6CB00, 0xF120, 4,
         kConfigFloatPrewarmKeys.data(), kConfigFloatPrewarmKeys.size()},
        {0xB6CB30, 0xB6CB38, 0xB6CB40, 0xF320, 1,
         kConfigKeyStatePrewarmKeys.data(),
         kConfigKeyStatePrewarmKeys.size()},
    }};
    std::array<ConfigRegistrySnapshot, specs.size()> before{};
    for (std::size_t registry_index = 0;
         registry_index != specs.size(); ++registry_index) {
        if (!ReadConfigRegistrySnapshot(
                process, image_base, specs[registry_index],
                before[registry_index], failure, registry_index,
                "config_prewarm_before"))
            return false;
    }
    void* remote_page = VirtualAllocEx(
        process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_page)
        return Fail(failure, "config_prewarm", "remote_allocate", 0, 0,
                    0, 1, GetLastError());
    bool success = true;
    for (std::size_t registry_index = 0;
         success && registry_index != specs.size(); ++registry_index) {
        const auto& spec = specs[registry_index];
        for (std::size_t key_index = 0;
             success && key_index != spec.key_count; ++key_index) {
            if (FindConfigRegistryEntry(before[registry_index],
                                        spec.keys[key_index]))
                continue;
            success = AccessConfigRegistryKey(
                process, image_base, spec, spec.keys[key_index], remote_page,
                failure, registry_index * 100 + key_index);
        }
    }
    std::array<ConfigRegistrySnapshot, specs.size()> after{};
    for (std::size_t registry_index = 0;
         success && registry_index != specs.size(); ++registry_index) {
        success = ReadConfigRegistrySnapshot(
            process, image_base, specs[registry_index],
            after[registry_index], failure, registry_index,
            "config_prewarm_after");
    }
    for (std::size_t registry_index = 0;
         success && registry_index != specs.size(); ++registry_index) {
        success = VerifyConfigRegistryPrewarm(
            specs[registry_index], before[registry_index],
            after[registry_index], failure, registry_index);
        if (success)
            std::printf("kirkware config prewarm registry=%llu "
                        "before=%llu after=%llu\n",
                        static_cast<unsigned long long>(registry_index),
                        static_cast<unsigned long long>(
                            before[registry_index].count),
                        static_cast<unsigned long long>(
                            after[registry_index].count));
    }
    const DWORD free_error = VirtualFreeEx(process, remote_page, 0,
                                           MEM_RELEASE)
                                 ? ERROR_SUCCESS
                                 : GetLastError();
    if (success && free_error != ERROR_SUCCESS)
        return Fail(failure, "config_prewarm", "remote_free", 0,
                    reinterpret_cast<std::uint64_t>(remote_page), 0, 1,
                    free_error);
    return success;
}

bool RollbackConfigBindings(HANDLE process, std::uint64_t image_base,
                            ConfigBindingsState& state) {
    constexpr std::uint32_t vector_rva = 0xB6B3F8;
    constexpr std::size_t record_size = 0x50;
    constexpr std::size_t maximum_count = 48;
    constexpr std::array<std::uint8_t, 16> destructor_prefix{{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83,
        0xEC, 0x30, 0x48, 0x8B, 0x19, 0x48, 0x8B, 0xF1}};
    if (state.preexisting || state.preserve_on_rollback ||
        !state.initialization_attempted ||
        !state.mutation_possible)
        return true;
    if (!RemoteCallsRollbackSafe())
        return false;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    std::uint64_t file_find = 0;
    std::uint64_t render_find = 0;
    auto read_state = [&]() {
        return ReadValue(process, image_base + vector_rva, begin) &&
               ReadValue(process, image_base + vector_rva + 8, end) &&
               ReadValue(process, image_base + vector_rva + 16,
                         capacity) &&
               ReadValue(process, image_base + 0xB6B540, file_find) &&
               ReadValue(process, image_base + 0xB6B548, render_find);
    };
    auto valid_vector = [&]() {
        if (!begin && !end && !capacity)
            return true;
        if (!begin || end < begin || capacity < end ||
            (end - begin) % record_size != 0 ||
            (capacity - begin) % record_size != 0)
            return false;
        const std::uint64_t used = end - begin;
        return used <= maximum_count * record_size &&
               (!used || ReadableRange(process, begin,
                                        static_cast<std::size_t>(used)));
    };
    if (!read_state() || !valid_vector())
        return false;
    const std::size_t count = begin
        ? static_cast<std::size_t>((end - begin) / record_size)
        : 0;
    if (count == state.count_before && file_find == state.file_find_before &&
        render_find == state.render_find_before)
        return true;
    if (state.count_before != 0 || state.file_find_before != 0 ||
        state.render_find_before != 0)
        return false;
    if (begin) {
        const std::uint64_t destructor = image_base + 0xCF050;
        Failure ignored{};
        if (!ExecutableRange(process, destructor, destructor_prefix.size()) ||
            !VerifyBytes(process, destructor, destructor_prefix.data(),
                         destructor_prefix.size(), ignored,
                         "config_bindings_rollback", "destructor_prefix", 0))
            return false;
        std::uint64_t result = UINT64_MAX;
        const bool called = RemoteCall3(
            process, destructor, image_base + vector_rva, 0, 0, result,
            ignored, "config_bindings_rollback", 0);
        if (!RemoteCallsRollbackSafe())
            return false;
        if (!called && (!read_state() || begin || end || capacity))
            return false;
    }
    if (!WriteProtectedQword(process, image_base + 0xB6B540,
                             state.file_find_before) ||
        !WriteProtectedQword(process, image_base + 0xB6B548,
                             state.render_find_before) ||
        !read_state() || begin || end || capacity || file_find || render_find)
        return false;
    state.mutation_possible = false;
    return true;
}

struct DynamicMinHookState {
    std::uint64_t target = 0;
    std::uint64_t saved = 0;
    std::uint64_t target_slot = 0;
    std::uint64_t saved_slot = 0;
    std::uint64_t detour = 0;
    std::uint64_t relay = 0;
    std::uint32_t cleanup_rva = 0;
    std::array<std::uint8_t, kTargetPrefixSize> target_prefix{};
    std::array<std::uint8_t, kDetourPrefixSize> detour_prefix{};
    std::array<std::uint8_t, kDetourPrefixSize> cleanup_prefix{};
    bool has_target_slot = false;
    bool preexisting = false;
    bool creation_attempted = false;
    bool created = false;
    bool prefix_known = false;
};

bool VerifyDynamicMinHook(HANDLE process, DynamicMinHookState& state,
                          Failure& failure, std::size_t index,
                          const char* stage);

bool EnsureMouseInputBinding(HANDLE process, std::uint64_t image_base,
                             DynamicMinHookState& state,
                             Failure& failure) {
    constexpr std::uint32_t initializer_rva = 0x2F3100;
    constexpr std::uint32_t saved_rva = 0x7EA900;
    constexpr std::array<std::uint8_t, 16> initializer_prefix{{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x48, 0x8D,
        0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xE0, 0x00}};
    constexpr std::array<std::uint8_t, 16> detour_prefix{{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
        0x89, 0x70, 0x10, 0xF3, 0x0F, 0x11, 0x58, 0x20}};
    state.saved_slot = image_base + saved_rva;
    state.detour = image_base + 0x2F1BD0;
    state.detour_prefix = detour_prefix;
    std::uint64_t saved = 0;
    if (!ReadValue(process, state.saved_slot, saved))
        return Fail(failure, "mouse_input_binding", "saved_read", 0,
                    state.saved_slot, 0, 0, GetLastError());
    state.saved = saved;
    state.preexisting = saved != 0;
    if (saved)
        return VerifyDynamicMinHook(process, state, failure, 0,
                                    "mouse_input_binding_pre");
    const std::uint64_t initializer = image_base + initializer_rva;
    if (!ExecutableRange(process, initializer, initializer_prefix.size()) ||
        !VerifyBytes(process, initializer, initializer_prefix.data(),
                     initializer_prefix.size(), failure,
                     "mouse_input_binding", "initializer_prefix", 0) ||
        !ExecutableRange(process, state.detour, detour_prefix.size()) ||
        !VerifyBytes(process, state.detour, detour_prefix.data(),
                     detour_prefix.size(), failure,
                     "mouse_input_binding", "detour_prefix", 0))
        return false;
    state.creation_attempted = true;
    state.created = true;
    std::uint64_t result = UINT64_MAX;
    const bool call_ok =
        RemoteCall3(process, initializer, 0, 0, 0, result, failure,
                    "mouse_input_binding_call", 0);
    const bool read_ok = ReadValue(process, state.saved_slot, saved);
    state.saved = read_ok ? saved : 0;
    if (!call_ok) {
        if (read_ok && saved) {
            Failure ignored{};
            VerifyDynamicMinHook(process, state, ignored, 0,
                                 "mouse_input_binding_recovery");
        }
        return false;
    }
    if (!read_ok || !saved)
        return Fail(failure, "mouse_input_binding", "exact_poststate", 0,
                    state.saved_slot, saved, 1, GetLastError());
    return VerifyDynamicMinHook(process, state, failure, 0,
                                "mouse_input_binding_post");
}

bool EnsureDynamicMinHookBindings(HANDLE process,
                                   std::uint64_t image_base,
                                   std::array<DynamicMinHookState, 2>& states,
                                   Failure& failure) {
    struct Binding {
        std::uint32_t initializer_rva;
        std::uint32_t target_rva;
        std::uint32_t saved_rva;
        std::uint32_t detour_rva;
        std::uint32_t cleanup_rva;
        std::array<std::uint8_t, 8> initializer_prefix;
        std::array<std::uint8_t, kDetourPrefixSize> detour_prefix;
        std::array<std::uint8_t, kDetourPrefixSize> cleanup_prefix;
    };
    constexpr std::array<Binding, 2> bindings{{
        {0x31B3D0, 0xB6BA50, 0xB6BA48, 0x31B370, 0x31B590,
         {0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48},
         {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
          0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57},
         {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0xB5,
          0x04, 0x85, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x23}},
        {0x31B630, 0xB6BA60, 0xB6BA58, 0x31B5D0, 0x31B830,
         {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x48, 0x8D},
         {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
          0xD9, 0x48, 0x8B, 0x0D, 0xC8, 0xF2, 0x4C, 0x00},
         {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0x25,
          0x02, 0x85, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x23}},
    }};
    for (std::size_t index = 0; index != bindings.size(); ++index) {
        const auto& binding = bindings[index];
        auto& state = states[index];
        const std::uint64_t initializer =
            image_base + binding.initializer_rva;
        state.target_slot = image_base + binding.target_rva;
        state.saved_slot = image_base + binding.saved_rva;
        state.detour = image_base + binding.detour_rva;
        state.cleanup_rva = binding.cleanup_rva;
        state.detour_prefix = binding.detour_prefix;
        state.cleanup_prefix = binding.cleanup_prefix;
        state.has_target_slot = true;
        std::uint64_t target = 0;
        std::uint64_t saved = 0;
        if (!ReadValue(process, state.target_slot, target) ||
            !ReadValue(process, state.saved_slot, saved))
            return Fail(failure, "dynamic_bindings", "state_read", index,
                        state.target_slot, target, saved,
                        GetLastError());
        state.target = target;
        state.saved = saved;
        state.preexisting = target != 0 || saved != 0;
        if (target || saved) {
            if (!target || !saved)
                return Fail(failure, "dynamic_bindings",
                            "installed_consistent", index,
                            state.saved_slot, saved, target,
                            GetLastError());
            if (!VerifyDynamicMinHook(process, state, failure, index,
                                      "dynamic_bindings_pre"))
                return false;
            continue;
        }
        if (!ExecutableRange(process, initializer,
                              binding.initializer_prefix.size()) ||
            !VerifyBytes(process, initializer,
                         binding.initializer_prefix.data(),
                         binding.initializer_prefix.size(), failure,
                         "dynamic_bindings", "initializer_prefix", index))
            return false;
        if (!ExecutableRange(process, state.detour,
                             state.detour_prefix.size()) ||
            !VerifyBytes(process, state.detour, state.detour_prefix.data(),
                         state.detour_prefix.size(), failure,
                         "dynamic_bindings", "detour_prefix", index))
            return false;
        if (!ExecutableRange(process, image_base + state.cleanup_rva,
                             state.cleanup_prefix.size()) ||
            !VerifyBytes(process, image_base + state.cleanup_rva,
                         state.cleanup_prefix.data(),
                         state.cleanup_prefix.size(), failure,
                         "dynamic_bindings", "cleanup_prefix", index))
            return false;
        state.creation_attempted = true;
        state.created = true;
        std::uint64_t result = UINT64_MAX;
        const bool call_ok =
            RemoteCall3(process, initializer, 0, 0, 0, result, failure,
                        "dynamic_bindings_call", index);
        const bool target_read = ReadValue(process, state.target_slot, target);
        const bool saved_read = ReadValue(process, state.saved_slot, saved);
        state.target = target_read ? target : 0;
        state.saved = saved_read ? saved : 0;
        if (!call_ok) {
            if (target_read && saved_read && target && saved) {
                Failure ignored{};
                VerifyDynamicMinHook(process, state, ignored, index,
                                     "dynamic_bindings_recovery");
            }
            return false;
        }
        if (!target_read || !saved_read || !target || !saved)
            return Fail(failure, "dynamic_bindings", "exact_poststate",
                        index, state.saved_slot, saved, target,
                        GetLastError());
        if (!VerifyDynamicMinHook(process, state, failure, index,
                                  "dynamic_bindings_post"))
            return false;
    }
    return true;
}

bool ReadLuaRegistrarVector(HANDLE process, std::uint64_t address,
                            LuaRegistrarVector& vector, Failure& failure,
                            const char* predicate) {
    if (!ReadValue(process, address, vector.begin) ||
        !ReadValue(process, address + 8, vector.end) ||
        !ReadValue(process, address + 16, vector.capacity))
        return Fail(failure, "lua_linker", predicate, 0, address,
                    vector.begin, vector.end, GetLastError());
    if (!vector.begin)
        return !vector.end && !vector.capacity
                   ? true
                   : Fail(failure, "lua_linker", predicate, 0, address,
                          vector.end, vector.capacity);
    if (vector.end < vector.begin || vector.capacity < vector.end ||
        ((vector.end - vector.begin) % 0x28) != 0 ||
        ((vector.capacity - vector.begin) % 0x28) != 0 ||
        (vector.end - vector.begin) / 0x28 > 4096 ||
        (vector.end != vector.begin &&
         !ReadableRange(process, vector.begin, vector.end - vector.begin)))
        return Fail(failure, "lua_linker", predicate, 0, address,
                    vector.begin, vector.end);
    return true;
}

constexpr std::size_t kLuaRegistrarRecordSize = 0x28;
constexpr std::size_t kLuaModuleRegistrarCount = 26;

struct LuaRegistrarRecord {
    std::array<char, 16> storage{};
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
    std::uint64_t callback = 0;
};

static_assert(sizeof(LuaRegistrarRecord) == kLuaRegistrarRecordSize);

struct LuaRegistrarSpec {
    const char* name;
    std::uint32_t callback_rva;
};

constexpr std::array<LuaRegistrarSpec, kLuaModuleRegistrarCount>
    kLuaModuleRegistrarSpecs{{
        {"reflection", 0x5F5D0},
        {"signal", 0x14E120},
        {"base", 0x63BD0},
        {"coroutine", 0x6B910},
        {"string", 0x1621A0},
        {"debug", 0x77B80},
        {"table", 0x1622B0},
        {"math", 0x12DCA0},
        {"bit", 0x63EB0},
        {"os", 0x12E050},
        {"fs", 0x84660},
        {"lxz", 0x6A8C0},
        {"iot", 0xFCDA0},
        {"paint", 0x148350},
        {"sodium", 0x161AF0},
        {"linker", 0x9C940},
        {"ui", 0xB5750},
        {"config", 0xB86C0},
        {"playerlist", 0xB8E30},
        {"steam", 0xB9860},
        {"net", 0xBC220},
        {"automation", 0xC2520},
        {"ec", 0xC8770},
        {"console", 0xCA490},
        {"misc", 0xC5160},
        {"listener", 0x336D60},
    }};

constexpr LuaRegistrarSpec kLuaRestrictedLinkerSpec{"linker", 0x9CCE0};

enum class LuaModuleRegistrarLayout {
    Empty,
    CanonicalPrefix,
    MissingLinker,
    LinkerAtTail,
    Canonical,
};

enum class LuaRestrictedRegistrarLayout {
    Empty,
    Canonical,
};

bool SameLuaRegistrarVector(const LuaRegistrarVector& left,
                            const LuaRegistrarVector& right) {
    return left.begin == right.begin && left.end == right.end &&
           left.capacity == right.capacity;
}

std::size_t LuaRegistrarCount(const LuaRegistrarVector& vector) {
    return vector.begin
               ? static_cast<std::size_t>((vector.end - vector.begin) /
                                          kLuaRegistrarRecordSize)
               : 0;
}

bool ReadLuaRegistrarRecords(
    HANDLE process, const LuaRegistrarVector& vector,
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& records,
    std::size_t& count, Failure& failure, const char* predicate) {
    records = {};
    count = LuaRegistrarCount(vector);
    if (count > records.size())
        return Fail(failure, "lua_linker", predicate, 0, vector.begin,
                    count, records.size());
    if (count &&
        !ReadExact(process, vector.begin, records.data(),
                   count * sizeof(records[0])))
        return Fail(failure, "lua_linker", predicate, 0, vector.begin,
                    0, count * sizeof(records[0]), GetLastError());
    return true;
}

const LuaRegistrarSpec& LuaRegistrarExpectedSpec(
    LuaModuleRegistrarLayout layout, std::size_t index) {
    if (layout == LuaModuleRegistrarLayout::MissingLinker)
        return kLuaModuleRegistrarSpecs[index < 15 ? index : index + 1];
    if (layout == LuaModuleRegistrarLayout::LinkerAtTail) {
        if (index < 15)
            return kLuaModuleRegistrarSpecs[index];
        if (index < 25)
            return kLuaModuleRegistrarSpecs[index + 1];
        return kLuaModuleRegistrarSpecs[15];
    }
    return kLuaModuleRegistrarSpecs[index];
}

std::size_t LuaRegistrarExpectedCount(LuaModuleRegistrarLayout layout) {
    if (layout == LuaModuleRegistrarLayout::Empty)
        return 0;
    return layout == LuaModuleRegistrarLayout::MissingLinker
               ? kLuaModuleRegistrarCount - 1
               : kLuaModuleRegistrarCount;
}

bool LuaRegistrarRecordMatches(const LuaRegistrarRecord& record,
                               const LuaRegistrarSpec& spec,
                               std::uint64_t image_base) {
    const std::size_t expected_size = std::strlen(spec.name);
    return expected_size < record.storage.size() &&
           record.capacity == 15 && record.size == expected_size &&
           std::memcmp(record.storage.data(), spec.name, expected_size) == 0 &&
           record.storage[expected_size] == 0 &&
           record.callback == image_base + spec.callback_rva;
}

bool LuaRegistrarLayoutMatches(
    const std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& records,
    std::size_t count, LuaModuleRegistrarLayout layout,
    std::uint64_t image_base) {
    if (count != LuaRegistrarExpectedCount(layout))
        return false;
    for (std::size_t index = 0; index != count; ++index) {
        if (!LuaRegistrarRecordMatches(
                records[index], LuaRegistrarExpectedSpec(layout, index),
                image_base))
            return false;
    }
    return true;
}

bool VerifyLuaRegistrarRecord(HANDLE process,
                              const LuaRegistrarRecord& record,
                              const LuaRegistrarSpec& spec,
                              std::uint64_t image_base,
                              std::uint64_t address, std::size_t index,
                              Failure& failure, const char* predicate) {
    const std::size_t expected_size = std::strlen(spec.name);
    if (expected_size >= record.storage.size())
        return Fail(failure, "lua_linker", predicate, index, address,
                    expected_size, record.storage.size() - 1);
    if (record.capacity != 15)
        return Fail(failure, "lua_linker", predicate, index,
                    address + 0x18, record.capacity, 15);
    if (record.size != expected_size)
        return Fail(failure, "lua_linker", predicate, index,
                    address + 0x10, record.size, expected_size);
    if (std::memcmp(record.storage.data(), spec.name, expected_size) != 0 ||
        record.storage[expected_size] != 0)
        return Fail(
            failure, "lua_linker", predicate, index, address,
            PrefixValue(
                reinterpret_cast<const std::uint8_t*>(record.storage.data()),
                record.storage.size()),
            PrefixValue(reinterpret_cast<const std::uint8_t*>(spec.name),
                        expected_size + 1));
    const std::uint64_t expected_callback =
        image_base + spec.callback_rva;
    if (record.callback != expected_callback)
        return Fail(failure, "lua_linker", predicate, index,
                    address + 0x20, record.callback, expected_callback);
    if (!ExecutableRange(process, record.callback, 5))
        return Fail(failure, "lua_linker", predicate, index,
                    address + 0x20, record.callback, 1, GetLastError());
    return true;
}

bool VerifyLuaRegistrarLayout(
    HANDLE process,
    const std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& records,
    std::size_t count, LuaModuleRegistrarLayout layout,
    std::uint64_t image_base, std::uint64_t begin, Failure& failure,
    const char* predicate) {
    const std::size_t expected_count = LuaRegistrarExpectedCount(layout);
    if (count != expected_count)
        return Fail(failure, "lua_linker", predicate, 0, begin, count,
                    expected_count);
    for (std::size_t index = 0; index != count; ++index) {
        if (!VerifyLuaRegistrarRecord(
                process, records[index],
                LuaRegistrarExpectedSpec(layout, index), image_base,
                begin + index * kLuaRegistrarRecordSize, index, failure,
                predicate))
            return false;
    }
    return true;
}

bool ReadLuaModuleRegistrarState(
    HANDLE process, std::uint64_t image_base, LuaRegistrarVector& vector,
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& records,
    std::size_t& count, LuaModuleRegistrarLayout& layout, Failure& failure,
    const char* predicate) {
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308, vector,
                                failure, predicate) ||
        !ReadLuaRegistrarRecords(process, vector, records, count, failure,
                                 predicate))
        return false;
    if (count == 0) {
        if (vector.begin || vector.end || vector.capacity)
            return Fail(failure, "lua_linker", predicate, 0,
                        image_base + 0xB6B308, vector.begin, 0);
        layout = LuaModuleRegistrarLayout::Empty;
        return true;
    }
    if (count < kLuaModuleRegistrarCount) {
        bool canonical_prefix = true;
        for (std::size_t index = 0; index != count; ++index) {
            if (!LuaRegistrarRecordMatches(records[index],
                                           kLuaModuleRegistrarSpecs[index],
                                           image_base)) {
                canonical_prefix = false;
                break;
            }
        }
        if (canonical_prefix) {
            for (std::size_t index = 0; index != count; ++index) {
                if (!VerifyLuaRegistrarRecord(
                        process, records[index],
                        kLuaModuleRegistrarSpecs[index], image_base,
                        vector.begin + index * kLuaRegistrarRecordSize,
                        index, failure, predicate))
                    return false;
            }
            layout = LuaModuleRegistrarLayout::CanonicalPrefix;
            return true;
        }
    }
    if (count == kLuaModuleRegistrarCount - 1) {
        if (!VerifyLuaRegistrarLayout(
                process, records, count,
                LuaModuleRegistrarLayout::MissingLinker, image_base,
                vector.begin, failure, predicate))
            return false;
        layout = LuaModuleRegistrarLayout::MissingLinker;
        return true;
    }
    if (count == kLuaModuleRegistrarCount) {
        if (LuaRegistrarLayoutMatches(records, count,
                                      LuaModuleRegistrarLayout::Canonical,
                                      image_base)) {
            layout = LuaModuleRegistrarLayout::Canonical;
            return true;
        }
        if (LuaRegistrarLayoutMatches(
                records, count, LuaModuleRegistrarLayout::LinkerAtTail,
                image_base)) {
            layout = LuaModuleRegistrarLayout::LinkerAtTail;
            return true;
        }
        const bool likely_tail =
            LuaRegistrarRecordMatches(records[15],
                                      kLuaModuleRegistrarSpecs[16],
                                      image_base);
        const auto observed_layout =
            likely_tail ? LuaModuleRegistrarLayout::LinkerAtTail
                        : LuaModuleRegistrarLayout::Canonical;
        if (!VerifyLuaRegistrarLayout(process, records, count,
                                      observed_layout, image_base,
                                      vector.begin, failure, predicate))
            return false;
    }
    return Fail(failure, "lua_linker", predicate, 0,
                image_base + 0xB6B308, count,
                kLuaModuleRegistrarCount);
}

bool ReadLuaRestrictedRegistrarState(
    HANDLE process, std::uint64_t image_base, LuaRegistrarVector& vector,
    LuaRestrictedRegistrarLayout& layout, Failure& failure,
    const char* predicate) {
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> records{};
    std::size_t count = 0;
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B2D8, vector,
                                failure, predicate) ||
        !ReadLuaRegistrarRecords(process, vector, records, count, failure,
                                 predicate))
        return false;
    if (!vector.begin && !vector.end && !vector.capacity) {
        layout = LuaRestrictedRegistrarLayout::Empty;
        return true;
    }
    if (count != 1)
        return Fail(failure, "lua_linker", predicate, 0,
                    image_base + 0xB6B2D8, count, 1);
    if (!VerifyLuaRegistrarRecord(process, records[0],
                                  kLuaRestrictedLinkerSpec, image_base,
                                  vector.begin, 0, failure, predicate))
        return false;
    layout = LuaRestrictedRegistrarLayout::Canonical;
    return true;
}

bool RestoreLuaRegistrarTail(
    HANDLE process, std::uint64_t image_base,
    const LuaRegistrarVector& expected_vector,
    const std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& snapshot) {
    LuaRegistrarVector vector{};
    Failure ignored{};
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308, vector,
                                ignored, "rotation_restore_vector") ||
        !SameLuaRegistrarVector(vector, expected_vector) ||
        !WritableRange(process, vector.begin, sizeof(snapshot)) ||
        !WriteExact(process, vector.begin, snapshot.data(),
                    sizeof(snapshot)))
        return false;
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> observed{};
    if (!ReadExact(process, vector.begin, observed.data(), sizeof(observed)) ||
        std::memcmp(observed.data(), snapshot.data(), sizeof(snapshot)) != 0)
        return false;
    LuaRegistrarVector final_vector{};
    return ReadLuaRegistrarVector(process, image_base + 0xB6B308,
                                  final_vector, ignored,
                                  "rotation_restore_final") &&
           SameLuaRegistrarVector(final_vector, expected_vector) &&
           LuaRegistrarLayoutMatches(
               observed, observed.size(),
               LuaModuleRegistrarLayout::LinkerAtTail, image_base);
}

bool RotateLuaRegistrarLinkerIntoCanonicalOrder(
    HANDLE process, std::uint64_t image_base,
    const LuaRegistrarVector& expected_vector,
    const std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>& expected,
    Failure& failure, bool& rollback_safe) {
    if (!VerifyLuaRegistrarLayout(
            process, expected, expected.size(),
            LuaModuleRegistrarLayout::LinkerAtTail, image_base,
            expected_vector.begin, failure, "rotation_prestate") ||
        !WritableRange(process, expected_vector.begin, sizeof(expected)))
        return Fail(failure, "lua_linker", "rotation_writable", 0,
                    expected_vector.begin, 0, sizeof(expected),
                    GetLastError());
    LuaRegistrarVector vector{};
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> snapshot{};
    Failure observed_failure{};
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308, vector,
                                observed_failure,
                                "rotation_vector_stable") ||
        !SameLuaRegistrarVector(vector, expected_vector) ||
        !ReadExact(process, vector.begin, snapshot.data(),
                   sizeof(snapshot)) ||
        std::memcmp(snapshot.data(), expected.data(), sizeof(snapshot)) != 0)
        return Fail(failure, "lua_linker", "rotation_stable", 0,
                    image_base + 0xB6B308, vector.begin,
                    expected_vector.begin, GetLastError());
    LuaRegistrarVector second_vector{};
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> second{};
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308,
                                second_vector, observed_failure,
                                "rotation_vector_second") ||
        !SameLuaRegistrarVector(second_vector, expected_vector) ||
        !ReadExact(process, second_vector.begin, second.data(),
                   sizeof(second)) ||
        std::memcmp(second.data(), snapshot.data(), sizeof(second)) != 0)
        return Fail(failure, "lua_linker", "rotation_second_stable", 0,
                    image_base + 0xB6B308, second_vector.begin,
                    expected_vector.begin, GetLastError());
    auto canonical = snapshot;
    canonical[15] = snapshot[25];
    for (std::size_t index = 15; index != 25; ++index)
        canonical[index + 1] = snapshot[index];
    if (!LuaRegistrarLayoutMatches(canonical, canonical.size(),
                                   LuaModuleRegistrarLayout::Canonical,
                                   image_base))
        return Fail(failure, "lua_linker", "rotation_candidate", 0,
                    expected_vector.begin, 0, 1);
    const bool wrote = WriteExact(process, expected_vector.begin,
                                  canonical.data(), sizeof(canonical));
    const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
    auto restore_or_fail = [&](const char* predicate, DWORD error) {
        if (!RestoreLuaRegistrarTail(process, image_base, expected_vector,
                                     snapshot)) {
            rollback_safe = false;
            return Fail(failure, "lua_linker", "rotation_restore", 0,
                        expected_vector.begin, 0, 1,
                        error ? error : ERROR_WRITE_FAULT);
        }
        return Fail(failure, "lua_linker", predicate, 0,
                    expected_vector.begin, 0, sizeof(canonical), error);
    };
    if (!wrote)
        return restore_or_fail("rotation_write", write_error);
    LuaRegistrarVector observed_vector{};
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> observed{};
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308,
                                observed_vector, observed_failure,
                                "rotation_post_vector"))
        return restore_or_fail("rotation_post_vector", GetLastError());
    if (!SameLuaRegistrarVector(observed_vector, expected_vector)) {
        rollback_safe = false;
        return Fail(failure, "lua_linker", "rotation_header_changed", 0,
                    image_base + 0xB6B308, observed_vector.begin,
                    expected_vector.begin);
    }
    if (!ReadExact(process, observed_vector.begin, observed.data(),
                   sizeof(observed)) ||
        std::memcmp(observed.data(), canonical.data(), sizeof(observed)) != 0)
        return restore_or_fail("rotation_bytes_exact", GetLastError());
    Failure semantic_failure{};
    if (!VerifyLuaRegistrarLayout(
            process, observed, observed.size(),
            LuaModuleRegistrarLayout::Canonical, image_base,
            observed_vector.begin, semantic_failure,
            "rotation_semantic_exact"))
        return restore_or_fail("rotation_semantic_exact",
                               semantic_failure.error);
    LuaRegistrarVector final_vector{};
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308,
                                final_vector, observed_failure,
                                "rotation_final_vector") ||
        !SameLuaRegistrarVector(final_vector, expected_vector)) {
        rollback_safe = false;
        return Fail(failure, "lua_linker", "rotation_final_header", 0,
                    image_base + 0xB6B308, final_vector.begin,
                    expected_vector.begin, GetLastError());
    }
    return true;
}

bool ValidateLuaSourceString(HANDLE process, std::uint64_t address,
                             Failure& failure, const char* predicate,
                             std::size_t index) {
    if (!address)
        return Fail(failure, "lua_linker_source", predicate, index,
                    address, 0, 1);
    std::uint64_t cursor = address;
    std::size_t remaining = 4096;
    while (remaining) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return Fail(failure, "lua_linker_source", predicate, index,
                        cursor, memory.State, MEM_COMMIT, GetLastError());
        const auto region =
            reinterpret_cast<std::uint64_t>(memory.BaseAddress);
        if (region > cursor || memory.RegionSize > UINT64_MAX - region)
            return Fail(failure, "lua_linker_source", predicate, index,
                        cursor, region, 0);
        const std::uint64_t region_end = region + memory.RegionSize;
        if (region_end <= cursor)
            return Fail(failure, "lua_linker_source", predicate, index,
                        cursor, region_end, cursor + 1);
        const std::size_t chunk = static_cast<std::size_t>(std::min<
            std::uint64_t>(remaining, std::min<std::uint64_t>(
                                           region_end - cursor, 256)));
        std::array<char, 256> value{};
        if (!ReadExact(process, cursor, value.data(), chunk))
            return Fail(failure, "lua_linker_source", predicate, index,
                        cursor, 0, chunk, GetLastError());
        if (std::memchr(value.data(), 0, chunk))
            return true;
        cursor += chunk;
        remaining -= chunk;
    }
    return Fail(failure, "lua_linker_source", predicate, index, address,
                4096, 4095);
}

bool ValidateLuaSourceChildren(HANDLE process, std::uint64_t object,
                               Failure& failure, std::size_t index) {
    std::uint64_t entries = 0;
    std::int32_t count = 0;
    if (!object || !ReadableRange(process, object, 0x18) ||
        !ReadValue(process, object, entries) ||
        !ReadValue(process, object + 0x10, count))
        return Fail(failure, "lua_linker_source", "children_header", index,
                    object, entries, static_cast<std::uint32_t>(count),
                    GetLastError());
    if (count < 0 || count > 1000)
        return Fail(failure, "lua_linker_source", "children_count", index,
                    object + 0x10, static_cast<std::uint32_t>(count), 1000);
    if (count &&
        (!entries || !ReadableRange(process, entries,
                                    static_cast<std::size_t>(count) * 8)))
        return Fail(failure, "lua_linker_source", "children_array", index,
                    entries, count, 1, GetLastError());
    for (std::int32_t child = 0; child < count; ++child) {
        std::uint64_t entry = 0;
        std::uint64_t name = 0;
        if (!ReadValue(process, entries + static_cast<std::uint64_t>(child) * 8,
                       entry) ||
            !entry || !ReadableRange(process, entry, 0x18) ||
            !ReadValue(process, entry, name))
            return Fail(failure, "lua_linker_source", "child_entry", index,
                        entry, static_cast<std::uint32_t>(child), count,
                        GetLastError());
        if (!ValidateLuaSourceString(process, name, failure, "child_name",
                                     index))
            return false;
    }
    return true;
}

bool ValidateLuaLinkerSources(HANDLE process, std::uint64_t list,
                              std::uint64_t buckets, Failure& failure) {
    std::uint64_t node = 0;
    if (!list || !ReadableRange(process, list, 8) ||
        !ReadValue(process, list, node))
        return Fail(failure, "lua_linker_source", "list_head", 0, list,
                    node, 1, GetLastError());
    std::vector<std::uint64_t> visited;
    visited.reserve(64);
    while (node && node != list) {
        if (visited.size() == 4096)
            return Fail(failure, "lua_linker_source", "list_limit", 0,
                        node, visited.size(), 4096);
        if (std::find(visited.begin(), visited.end(), node) != visited.end())
            return Fail(failure, "lua_linker_source", "list_cycle", 0,
                        node, visited.size(), 0);
        visited.push_back(node);
        std::uint64_t next = 0;
        std::uint64_t record = 0;
        if (!ReadableRange(process, node, 0x18) ||
            !ReadValue(process, node, next) ||
            !ReadValue(process, node + 0x10, record))
            return Fail(failure, "lua_linker_source", "list_node", 0,
                        node, next, record, GetLastError());
        if (record) {
            std::uint64_t name = 0;
            if (!ReadableRange(process, record, 0x30) ||
                !ReadValue(process, record + 0x10, name))
                return Fail(failure, "lua_linker_source", "list_record", 0,
                            record, name, 1, GetLastError());
            if (name &&
                (!ValidateLuaSourceString(process, name, failure,
                                          "list_name", 0) ||
                 !ValidateLuaSourceChildren(process, record + 0x18, failure,
                                            0)))
                return false;
        }
        node = next;
    }
    if (node && node != list)
        return Fail(failure, "lua_linker_source", "list_sentinel", 0,
                    node, list, list);
    if (!buckets || !ReadableRange(process, buckets, 35 * 8))
        return Fail(failure, "lua_linker_source", "bucket_array", 1,
                    buckets, 0, 35 * 8, GetLastError());
    for (std::size_t index = 0; index != 35; ++index) {
        std::uint64_t entry = 0;
        if (!ReadValue(process, buckets + index * 8, entry))
            return Fail(failure, "lua_linker_source", "bucket_read", index,
                        buckets + index * 8, entry, 1, GetLastError());
        if (!entry)
            continue;
        std::uint64_t name = 0;
        std::uint64_t children = 0;
        if (!ReadableRange(process, entry, 0x28) ||
            !ReadValue(process, entry + 8, name) ||
            !ReadValue(process, entry + 0x20, children))
            return Fail(failure, "lua_linker_source", "bucket_entry", index,
                        entry, name, children, GetLastError());
        if (!ValidateLuaSourceString(process, name, failure, "bucket_name",
                                     index) ||
            !ValidateLuaSourceChildren(process, children, failure, index))
            return false;
    }
    return true;
}

bool InvokeLuaLinkerCarrier(DWORD pid, std::uint64_t target,
                            Failure& failure, const char* predicate,
                            std::size_t index, bool& rollback_safe,
                            std::uint64_t rcx = 0,
                            std::uint64_t rdx = 0,
                            std::uint64_t r8 = 0) {
    char name[] = "carrier";
    char pid_text[32]{};
    char target_text[32]{};
    char rcx_text[32]{};
    char rdx_text[32]{};
    char r8_text[32]{};
    std::snprintf(pid_text, sizeof(pid_text), "%lu",
                  static_cast<unsigned long>(pid));
    std::snprintf(target_text, sizeof(target_text), "0x%llX",
                  static_cast<unsigned long long>(target));
    std::snprintf(rcx_text, sizeof(rcx_text), "0x%llX",
                  static_cast<unsigned long long>(rcx));
    std::snprintf(rdx_text, sizeof(rdx_text), "0x%llX",
                  static_cast<unsigned long long>(rdx));
    std::snprintf(r8_text, sizeof(r8_text), "0x%llX",
                  static_cast<unsigned long long>(r8));
    char* arguments[]{name, pid_text, target_text, rcx_text, rdx_text,
                      r8_text};
    rollback_safe = false;
    const int result = kirkware_carrier_entry(6, arguments);
    if (result != 0)
        return Fail(failure, "lua_linker", predicate, index, target,
                    static_cast<std::uint64_t>(result), 0);
    rollback_safe = true;
    return true;
}

struct LuaRegistrarVectorState {
    LuaRegistrarVector vector{};
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount> records{};
    std::size_t count = 0;
};

constexpr std::array<std::uint32_t, 10> kLuaBuiltinInitializers{{
    0x5F830, 0x14E1E0, 0x63E60, 0x6BB70, 0x162260,
    0x78C20, 0x1622E0, 0x12DDE0, 0x63EE0, 0x12E080,
}};

constexpr std::array<std::uint32_t, 6> kLuaNativeInitializers{{
    0x848C0, 0x6AA80, 0xFDF90, 0x149110, 0x162150, 0x9CD90,
}};

struct LuaNamedRegistrarSpec {
    const char* name;
    std::uint32_t callback_rva;
};

constexpr std::array<LuaNamedRegistrarSpec, 10> kLuaNamedRegistrars{{
    {"ui", 0xB5750},
    {"config", 0xB86C0},
    {"playerlist", 0xB8E30},
    {"steam", 0xB9860},
    {"net", 0xBC220},
    {"automation", 0xC2520},
    {"ec", 0xC8770},
    {"console", 0xCA490},
    {"misc", 0xC5160},
    {"listener", 0x336D60},
}};

constexpr std::array<std::uint32_t, 9> kLuaGlobalCallbacks{{
    0x786F0, 0x78740, 0x78790, 0x787E0, 0x78880,
    0x78920, 0x78A00, 0x78B20, 0x78BC0,
}};

bool ReadCanonicalLuaRegistrarPrefix(HANDLE process, std::uint64_t image_base,
                                     LuaRegistrarVectorState& state,
                                     Failure& failure,
                                     const char* predicate) {
    if (!ReadLuaRegistrarVector(process, image_base + 0xB6B308,
                                state.vector, failure, predicate) ||
        !ReadLuaRegistrarRecords(process, state.vector, state.records,
                                 state.count, failure, predicate))
        return false;
    if (state.count > kLuaModuleRegistrarCount)
        return Fail(failure, "lua_registrars", predicate, 0,
                    image_base + 0xB6B308, state.count,
                    kLuaModuleRegistrarCount);
    if (state.count == 0 &&
        (state.vector.begin || state.vector.end || state.vector.capacity))
        return Fail(failure, "lua_registrars", predicate, 0,
                    image_base + 0xB6B308, state.vector.begin, 0);
    for (std::size_t index = 0; index != state.count; ++index) {
        if (!VerifyLuaRegistrarRecord(
                process, state.records[index],
                kLuaModuleRegistrarSpecs[index], image_base,
                state.vector.begin + index * kLuaRegistrarRecordSize,
                index, failure, predicate))
            return false;
    }
    return true;
}

bool ReadExactLuaRegistrarCount(HANDLE process, std::uint64_t image_base,
                                std::uint32_t vector_rva,
                                std::size_t expected_count,
                                LuaRegistrarVector& vector,
                                Failure& failure,
                                const char* predicate) {
    if (!ReadLuaRegistrarVector(process, image_base + vector_rva, vector,
                                failure, predicate))
        return false;
    const std::size_t count = LuaRegistrarCount(vector);
    if (count != expected_count)
        return Fail(failure, "lua_registrars", predicate, 0,
                    image_base + vector_rva, count, expected_count);
    return true;
}

bool VerifyLuaCallbackVector(HANDLE process, std::uint64_t image_base,
                             std::uint32_t vector_rva,
                             const std::uint32_t* callbacks,
                             std::size_t expected_count,
                             Failure& failure,
                             const char* predicate) {
    LuaRegistrarVector vector{};
    if (!ReadExactLuaRegistrarCount(process, image_base, vector_rva,
                                    expected_count, vector, failure,
                                    predicate))
        return false;
    for (std::size_t index = 0; index != expected_count; ++index) {
        std::uint64_t observed = 0;
        const std::uint64_t address =
            vector.begin + index * kLuaRegistrarRecordSize + 0x20;
        const std::uint64_t expected = image_base + callbacks[index];
        if (!ReadValue(process, address, observed) || observed != expected)
            return Fail(failure, "lua_registrars", predicate, index,
                        address, observed, expected, GetLastError());
    }
    return true;
}

bool CallLuaNoArgumentRegistrar(DWORD pid, std::uint64_t image_base,
                                std::uint32_t rva, std::size_t index,
                                Failure& failure, bool& rollback_safe,
                                const char* predicate) {
    return InvokeLuaLinkerCarrier(pid, image_base + rva, failure, predicate,
                                  index, rollback_safe);
}

bool CallLuaNamedRegistrar(DWORD pid, HANDLE process,
                           std::uint64_t image_base, const char* name,
                           std::uint32_t callback_rva,
                           std::uint32_t registrar_rva,
                           std::size_t index, Failure& failure,
                           bool& rollback_safe,
                           const char* predicate) {
    struct RemoteSmallString {
        std::array<char, 16> storage{};
        std::uint64_t size = 0;
        std::uint64_t capacity = 15;
    };
    struct RemoteNamedBlock {
        RemoteSmallString value{};
        std::array<char, 64> source{};
    } block{};
    static_assert(sizeof(RemoteSmallString) == 32);
    const std::size_t size = std::strlen(name);
    if (size >= block.source.size())
        return Fail(failure, "lua_registrars", "name_bounded", index,
                    size, block.source.size() - 1);
    block.value.size = size;
    const bool long_name = size >= block.value.storage.size();
    if (!long_name) {
        std::memcpy(block.value.storage.data(), name, size);
    } else {
        block.value.size = 0;
        block.value.capacity = 15;
    }
    std::memcpy(block.source.data(), name, size);
    void* remote = VirtualAllocEx(process, nullptr, sizeof(block),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return Fail(failure, "lua_registrars", "name_allocate", index, 0,
                    0, sizeof(block), GetLastError());
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(remote);
    if (!WriteExact(process, address, &block, sizeof(block))) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return Fail(failure, "lua_registrars", "name_write", index,
                    address, 0, sizeof(block), error);
    }
    if (long_name &&
        !InvokeLuaLinkerCarrier(
            pid, image_base + 0xCA20, failure, "name_construct", index,
            rollback_safe, address,
            address + offsetof(RemoteNamedBlock, source), 0))
        return false;
    const bool called = InvokeLuaLinkerCarrier(
        pid, image_base + registrar_rva, failure, predicate, index,
        rollback_safe, address, image_base + callback_rva, 0);
    if (!called || !rollback_safe)
        return false;
    if (!VirtualFreeEx(process, remote, 0, MEM_RELEASE))
        return Fail(failure, "lua_registrars", "name_free", index,
                    address, 0, 1, GetLastError());
    return true;
}

bool ReadLuaMapCount(HANDLE process, std::uint64_t image_base,
                     std::uint32_t map_rva, std::uint64_t& count,
                     Failure& failure, const char* predicate,
                     std::size_t index) {
    std::uint64_t sentinel = 0;
    if (!ReadValue(process, image_base + map_rva, sentinel) ||
        !ReadValue(process, image_base + map_rva + 8, count) ||
        !sentinel || !ReadableRange(process, sentinel, 0x20))
        return Fail(failure, "lua_registrars", predicate, index,
                    image_base + map_rva, count, 1, GetLastError());
    return true;
}

bool EnsureLuaNamedMapEntry(DWORD pid, HANDLE process,
                            std::uint64_t image_base,
                            std::uint32_t map_rva,
                            std::uint32_t registrar_rva,
                            std::uint32_t callback_rva,
                            std::size_t index, Failure& failure,
                            bool& rollback_safe,
                            const char* predicate) {
    std::uint64_t count = 0;
    if (!ReadLuaMapCount(process, image_base, map_rva, count, failure,
                         predicate, index) || count > 1)
        return count > 1
                   ? Fail(failure, "lua_registrars", predicate, index,
                          image_base + map_rva + 8, count, 1)
                   : false;
    if (count == 0 &&
        !CallLuaNamedRegistrar(pid, process, image_base, "interface",
                               callback_rva, registrar_rva, index, failure,
                               rollback_safe, predicate))
        return false;
    if (!ReadLuaMapCount(process, image_base, map_rva, count, failure,
                         predicate, index) || count != 1)
        return Fail(failure, "lua_registrars", predicate, index,
                    image_base + map_rva + 8, count, 1, GetLastError());
    return true;
}

bool EnsureLuaApiEntry(DWORD pid, HANDLE process, std::uint64_t image_base,
                       const char* name, std::uint32_t callback_rva,
                       std::uint64_t expected_before,
                       std::uint64_t expected_after, std::size_t index,
                       Failure& failure, bool& rollback_safe,
                       const char* predicate) {
    std::uint64_t count = 0;
    if (!ReadValue(process, image_base + 0xB69488, count))
        return Fail(failure, "lua_registrars", predicate, index,
                    image_base + 0xB69488, count, expected_after,
                    GetLastError());
    if (count < expected_before || count > 3)
        return Fail(failure, "lua_registrars", predicate, index,
                    image_base + 0xB69488, count, expected_before);
    if (count == expected_before &&
        !CallLuaNamedRegistrar(pid, process, image_base, name, callback_rva,
                               0x57550, index, failure, rollback_safe,
                               predicate))
        return false;
    if (!ReadValue(process, image_base + 0xB69488, count) ||
        count < expected_after || count > 3)
        return Fail(failure, "lua_registrars", predicate, index,
                    image_base + 0xB69488, count, expected_after,
                    GetLastError());
    return true;
}

bool EnsureLuaConsoleRegistrars(DWORD pid, HANDLE process,
                                std::uint64_t image_base, Failure& failure,
                                bool& rollback_safe) {
    constexpr std::array<LuaNamedRegistrarSpec, 4> commands{{
        {"lua_openscript", 0xC8BC0},
        {"lua_openscript_cl", 0xC8FB0},
        {"lua_run", 0xC93A0},
        {"lua_run_cl", 0xC9520},
    }};
    std::uint64_t count = 0;
    if (!ReadLuaMapCount(process, image_base, 0xB69658, count, failure,
                         "console_prestate", 0) || count > commands.size())
        return count > commands.size()
                   ? Fail(failure, "lua_registrars", "console_prestate", 0,
                          image_base + 0xB69660, count, commands.size())
                   : false;
    for (std::size_t index = static_cast<std::size_t>(count);
         index != commands.size(); ++index) {
        if (!CallLuaNamedRegistrar(
                pid, process, image_base, commands[index].name,
                commands[index].callback_rva, 0xC5670, index, failure,
                rollback_safe, "console_register") ||
            !ReadValue(process, image_base + 0xB69660, count) ||
            count != index + 1)
            return Fail(failure, "lua_registrars", "console_increment",
                        index, image_base + 0xB69660, count, index + 1,
                        GetLastError());
    }
    return true;
}

bool EnsureLuaRegistrarContainers(DWORD pid, HANDLE process,
                                  std::uint64_t image_base,
                                  std::size_t module_count,
                                  Failure& failure,
                                  bool& rollback_safe) {
    constexpr std::array<std::uint32_t, 4> map_rvas{{
        0xB69480, 0xB694F0, 0xB69470, 0xB69500,
    }};
    std::size_t present = 0;
    for (const auto rva : map_rvas) {
        std::uint64_t value = 0;
        if (!ReadValue(process, image_base + rva, value))
            return Fail(failure, "lua_registrars", "container_read", 0,
                        image_base + rva, value, 1, GetLastError());
        if (value)
            ++present;
    }
    const bool initialize_containers = present != map_rvas.size();
    if (initialize_containers) {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t capacity = 0;
        if (module_count != 0 ||
            !ReadValue(process, image_base + 0xB6B338, begin) ||
            !ReadValue(process, image_base + 0xB6B340, end) ||
            !ReadValue(process, image_base + 0xB6B348, capacity) ||
            begin || end || capacity)
            return Fail(failure, "lua_registrars", "container_pristine", 0,
                        image_base + 0xB6B338, begin, 0, GetLastError());
        for (std::size_t index = 0; index != map_rvas.size(); ++index) {
            const std::uint64_t address = image_base + map_rvas[index];
            std::uint64_t sentinel = 0;
            std::uint64_t count = 0;
            if (!ReadValue(process, address, sentinel) ||
                !ReadValue(process, address + 8, count) || count != 0)
                return Fail(failure, "lua_registrars",
                            "container_partial_count", index, address,
                            count, 0, GetLastError());
            if (!sentinel)
                continue;
            std::uint64_t left = 0;
            std::uint64_t parent = 0;
            std::uint64_t right = 0;
            std::uint16_t flags = 0;
            if (!ReadableRange(process, sentinel, 0x1A) ||
                !ReadValue(process, sentinel, left) ||
                !ReadValue(process, sentinel + 8, parent) ||
                !ReadValue(process, sentinel + 16, right) ||
                !ReadValue(process, sentinel + 24, flags) ||
                left != sentinel || parent != sentinel ||
                right != sentinel || flags != 0x0101)
                return Fail(failure, "lua_registrars",
                            "container_partial_empty", index, sentinel,
                            flags, 0x0101, GetLastError());
        }
        if (!CallLuaNoArgumentRegistrar(pid, image_base, 0x57590, 0,
                                        failure, rollback_safe,
                                        "container_initialize"))
            return false;
    }
    for (std::size_t index = 0; index != map_rvas.size(); ++index) {
        std::uint64_t value = 0;
        std::uint64_t count = 0;
        if (!ReadValue(process, image_base + map_rvas[index], value) ||
            !ReadValue(process, image_base + map_rvas[index] + 8, count) ||
            !value || !ReadableRange(process, value, 0x20) ||
            (initialize_containers && count != 0))
            return Fail(failure, "lua_registrars", "container_ready",
                        index, image_base + map_rvas[index], count,
                        initialize_containers ? 0 : 1,
                        GetLastError());
    }
    return true;
}

bool PrepareLuaLinkerOriginalBuilders(DWORD pid, HANDLE process,
                                      std::uint64_t image_base,
                                      Failure& failure,
                                      bool& rollback_safe) {
    constexpr std::array<std::uint64_t, 7> dependency_rvas{{
        0x7EA5F8, 0x7EA5F0, 0x7EA5B0, 0x7EA5C0,
        0x7EA610, 0x7EA5D0, 0x7EA5D8,
    }};
    for (std::size_t index = 0; index != dependency_rvas.size(); ++index) {
        std::uint64_t value = UINT64_MAX;
        if (!ReadValue(process, image_base + dependency_rvas[index], value))
            return Fail(failure, "lua_linker", "dependency_read", index,
                        image_base + dependency_rvas[index], value, 0,
                        GetLastError());
        if (value && !ExecutableRange(process, value, 5))
            return Fail(failure, "lua_linker",
                        "dependency_partial_executable", index,
                        image_base + dependency_rvas[index], value, 1,
                        GetLastError());
    }

    constexpr std::array<std::uint64_t, 2> singleton_rvas{{
        0x7EA5B8, 0x7EA5E0,
    }};
    constexpr std::array<std::uint64_t, 2> getter_rvas{{
        0x935E0, 0x93700,
    }};
    constexpr std::array<std::uint64_t, 2> singleton_guard_rvas{{
        0xB6E624, 0xB6E674,
    }};
    std::array<std::uint64_t, 2> singleton_values{};
    for (std::size_t index = 0; index != singleton_rvas.size(); ++index) {
        std::uint64_t singleton = 0;
        std::int32_t guard = 0;
        if (!ReadValue(process, image_base + singleton_rvas[index],
                       singleton) ||
            !ReadValue(process, image_base + singleton_guard_rvas[index],
                       guard))
            return Fail(failure, "lua_linker", "singleton_prestate", index,
                        image_base + singleton_rvas[index], singleton,
                        static_cast<std::uint32_t>(guard), GetLastError());
        if (guard == -1)
            return Fail(failure, "lua_linker", "singleton_busy", index,
                        image_base + singleton_guard_rvas[index],
                        static_cast<std::uint32_t>(guard), 0);
        const bool initialize_singleton = !singleton;
        if (initialize_singleton &&
            !InvokeLuaLinkerCarrier(pid, image_base + getter_rvas[index],
                                    failure, "singleton_initialize", index,
                                    rollback_safe))
            return false;
        if (!ReadValue(process, image_base + singleton_rvas[index],
                       singleton) ||
            !ReadValue(process, image_base + singleton_guard_rvas[index],
                       guard) ||
            !singleton || guard == -1 ||
            (initialize_singleton && guard == 0))
            return Fail(failure, "lua_linker", "singleton_ready", index,
                        image_base + singleton_rvas[index], singleton,
                        static_cast<std::uint32_t>(guard), GetLastError());
        singleton_values[index] = singleton;
    }
    if (!ValidateLuaLinkerSources(process, singleton_values[0],
                                  singleton_values[1], failure))
        return false;

    constexpr std::array<std::uint64_t, 2> builder_guard_rvas{{
        0xB6BCD8, 0xB6BCE8,
    }};
    constexpr std::array<std::uint64_t, 2> builder_rvas{{
        0x938A0, 0x93AA0,
    }};
    for (std::size_t index = 0; index != builder_rvas.size(); ++index) {
        const std::uint64_t guard_address =
            image_base + builder_guard_rvas[index];
        std::uint8_t guard = 0;
        if (!ReadValue(process, guard_address, guard) || guard > 1)
            return Fail(failure, "lua_linker", "builder_guard_prestate",
                        index, guard_address, guard, 1, GetLastError());
        if (guard == 0)
            continue;
        const std::uint8_t reset = 0;
        std::uint8_t observed = UINT8_MAX;
        if (!WritableRange(process, guard_address, sizeof(reset)) ||
            !WriteExact(process, guard_address, &reset, sizeof(reset)) ||
            !ReadValue(process, guard_address, observed) || observed != 0)
            return Fail(failure, "lua_linker", "builder_guard_reset", index,
                        guard_address, observed, 0, GetLastError());
        if (!InvokeLuaLinkerCarrier(pid, image_base + builder_rvas[index],
                                    failure, "builder_retry", index,
                                    rollback_safe))
            return false;
        observed = 0;
        if (!ReadValue(process, guard_address, observed) || observed != 1)
            return Fail(failure, "lua_linker", "builder_retry_complete",
                        index, guard_address, observed, 1, GetLastError());
    }
    return true;
}

bool EnsureCompleteLuaRegistrarChain(DWORD pid, HANDLE process,
                                     std::uint64_t image_base,
                                     Failure& failure,
                                     bool& rollback_safe) {
    std::uint64_t compression_baseline = 0;
    if (!ReadValue(process, image_base + 0xB69758,
                   compression_baseline) || compression_baseline != 5)
        return Fail(failure, "lua_registrars", "compression_prestate", 0,
                    image_base + 0xB69758, compression_baseline, 5,
                    GetLastError());
    auto verify_compression = [&](const char* predicate,
                                  std::size_t index) {
        std::uint64_t observed = 0;
        return ReadValue(process, image_base + 0xB69758, observed) &&
                       observed == compression_baseline
                   ? true
                   : Fail(failure, "lua_registrars", predicate, index,
                          image_base + 0xB69758, observed,
                          compression_baseline, GetLastError());
    };
    LuaRegistrarVectorState state{};
    if (!ReadCanonicalLuaRegistrarPrefix(process, image_base, state, failure,
                                         "module_prefix_prestate") ||
        !EnsureLuaRegistrarContainers(pid, process, image_base, state.count,
                                      failure, rollback_safe))
        return false;
    if (!EnsureLuaApiEntry(pid, process, image_base, "api", 0x59DF0, 0, 1,
                           0, failure, rollback_safe, "api_register"))
        return false;

    for (std::size_t index = state.count;
         index < kLuaBuiltinInitializers.size(); ++index) {
        if (!CallLuaNoArgumentRegistrar(
                pid, image_base, kLuaBuiltinInitializers[index], index,
                failure, rollback_safe, "builtin_register"))
            return false;
        if (!verify_compression("compression_after_builtin", index))
            return false;
        LuaRegistrarVectorState observed{};
        if (!ReadCanonicalLuaRegistrarPrefix(
                process, image_base, observed, failure,
                "builtin_poststate") || observed.count != index + 1)
            return Fail(failure, "lua_registrars", "builtin_increment",
                        index, image_base + 0xB6B308, observed.count,
                        index + 1, GetLastError());
        if (index == 1 &&
            !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB69888,
                                    0x14B0D0, 0x33A8E0, 0, failure,
                                    rollback_safe, "signal_interface"))
            return false;
    }
    if (state.count >= 2 &&
        !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB69888,
                                0x14B0D0, 0x33A8E0, 0, failure,
                                rollback_safe, "signal_interface"))
        return false;

    if (!ReadCanonicalLuaRegistrarPrefix(process, image_base, state, failure,
                                         "native_prestate"))
        return false;
    for (std::size_t module_index = state.count; module_index < 16;
         ++module_index) {
        const std::size_t native_index = module_index - 10;
        if (native_index == 5 &&
            !PrepareLuaLinkerOriginalBuilders(
                pid, process, image_base, failure, rollback_safe))
            return false;
        if (!CallLuaNoArgumentRegistrar(
                pid, image_base, kLuaNativeInitializers[native_index],
                module_index, failure, rollback_safe, "native_register"))
            return false;
        if (!verify_compression("compression_after_native", native_index))
            return false;
        LuaRegistrarVectorState observed{};
        if (!ReadCanonicalLuaRegistrarPrefix(
                process, image_base, observed, failure,
                "native_poststate") || observed.count != module_index + 1)
            return Fail(failure, "lua_registrars", "native_increment",
                        native_index, image_base + 0xB6B308,
                        observed.count, module_index + 1, GetLastError());
        if (native_index == 1 &&
            !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB69510,
                                    0x67710, 0x33B5E0, 1, failure,
                                    rollback_safe, "lxz_interface"))
            return false;
        if (native_index == 2 &&
            !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB696F8,
                                    0xE8C40, 0x33AE20, 2, failure,
                                    rollback_safe, "iot_interface"))
            return false;
    }
    if (!EnsureLuaNamedMapEntry(pid, process, image_base, 0xB69510,
                                0x67710, 0x33B5E0, 1, failure,
                                rollback_safe, "lxz_interface") ||
        !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB696F8,
                                0xE8C40, 0x33AE20, 2, failure,
                                rollback_safe, "iot_interface"))
        return false;
    if (!ReadCanonicalLuaRegistrarPrefix(process, image_base, state, failure,
                                         "api_phase_prestate"))
        return false;
    std::uint64_t api_count = 0;
    const bool api_phase_valid =
        ReadValue(process, image_base + 0xB69488, api_count) &&
        ((state.count < 22 && api_count == 2) ||
         (state.count == 22 && (api_count == 2 || api_count == 3)) ||
         (state.count > 22 && api_count == 3));
    if (!api_phase_valid)
        return Fail(failure, "lua_registrars", "iot_api_count", 0,
                    image_base + 0xB69488, api_count, 2, GetLastError());

    for (std::size_t module_index = state.count;
         module_index < kLuaModuleRegistrarCount; ++module_index) {
        const std::size_t loader_index = module_index - 16;
        if (loader_index == 6) {
            if (!EnsureLuaApiEntry(pid, process, image_base, "ec", 0xC7C00,
                                   2, 3, loader_index, failure,
                                   rollback_safe, "ec_api") ||
                !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB695B8,
                                        0xC5630, 0x33B200, loader_index,
                                        failure, rollback_safe,
                                        "ec_interface"))
                return false;
        }
        if (loader_index == 7 &&
            !EnsureLuaConsoleRegistrars(pid, process, image_base, failure,
                                        rollback_safe))
            return false;
        if (!CallLuaNamedRegistrar(
                pid, process, image_base, kLuaNamedRegistrars[loader_index].name,
                kLuaNamedRegistrars[loader_index].callback_rva, 0x599F0,
                loader_index, failure, rollback_safe, "loader_register"))
            return false;
        if (!verify_compression("compression_after_loader", loader_index))
            return false;
        LuaRegistrarVectorState observed{};
        if (!ReadCanonicalLuaRegistrarPrefix(
                process, image_base, observed, failure,
                "loader_poststate") || observed.count != module_index + 1)
            return Fail(failure, "lua_registrars", "loader_increment",
                        loader_index, image_base + 0xB6B308,
                        observed.count, module_index + 1, GetLastError());
    }
    if (!EnsureLuaApiEntry(pid, process, image_base, "ec", 0xC7C00,
                           2, 3, 6, failure, rollback_safe,
                           "ec_api_final") ||
        !EnsureLuaNamedMapEntry(pid, process, image_base, 0xB695B8,
                                0xC5630, 0x33B200, 6, failure,
                                rollback_safe, "ec_interface_final") ||
        !EnsureLuaConsoleRegistrars(pid, process, image_base, failure,
                                    rollback_safe))
        return false;
    LuaRegistrarVector restricted{};
    if (!ReadExactLuaRegistrarCount(process, image_base, 0xB6B2D8, 1,
                                    restricted, failure,
                                    "restricted_final") ||
        !VerifyLuaCallbackVector(process, image_base, 0xB6B2C0,
                                 kLuaGlobalCallbacks.data(),
                                 kLuaGlobalCallbacks.size(), failure,
                                 "globals_final"))
        return false;
    std::array<std::pair<std::uint32_t, std::uint64_t>, 7> final_counts{{
        {0xB69488, 3}, {0xB69518, 1}, {0xB695C0, 1},
        {0xB69890, 1}, {0xB69660, 4}, {0xB69700, 1},
        {0xB69758, 5},
    }};
    for (std::size_t index = 0; index != final_counts.size(); ++index) {
        std::uint64_t observed = 0;
        if (!ReadValue(process, image_base + final_counts[index].first,
                       observed) ||
            observed != final_counts[index].second)
            return Fail(failure, "lua_registrars", "final_aux_count",
                        index, image_base + final_counts[index].first,
                        observed, final_counts[index].second,
                        GetLastError());
    }
    return true;
}

bool EnsureLuaLinkerRegistrars(DWORD pid, HANDLE process,
                               std::uint64_t image_base, Failure& failure,
                               bool& rollback_safe) {
    LuaRegistrarVector module_vector{};
    LuaRegistrarVector restricted_vector{};
    std::array<LuaRegistrarRecord, kLuaModuleRegistrarCount>
        module_records{};
    std::size_t module_count = 0;
    LuaModuleRegistrarLayout module_layout{};
    LuaRestrictedRegistrarLayout restricted_layout{};
    if (!ReadLuaModuleRegistrarState(
            process, image_base, module_vector, module_records,
            module_count, module_layout, failure, "module_prestate") ||
        !ReadLuaRestrictedRegistrarState(
            process, image_base, restricted_vector, restricted_layout,
            failure, "restricted_prestate"))
        return false;
    constexpr std::array<std::uint64_t, 7> dependency_rvas{{
        0x7EA5F8, 0x7EA5F0, 0x7EA5B0, 0x7EA5C0,
        0x7EA610, 0x7EA5D0, 0x7EA5D8,
    }};
    auto verify_linker_runtime = [&](const char* dependency_predicate,
                                     const char* guard_predicate) {
        for (std::size_t index = 0; index != dependency_rvas.size();
             ++index) {
            std::uint64_t value = 0;
            if (!ReadValue(process,
                           image_base + dependency_rvas[index], value) ||
                !value || !ExecutableRange(process, value, 5))
                return Fail(failure, "lua_linker",
                            dependency_predicate, index,
                            image_base + dependency_rvas[index], value, 1,
                            GetLastError());
        }
        std::uint8_t first_guard = 0;
        std::uint8_t second_guard = 0;
        if (!ReadValue(process, image_base + 0xB6BCD8, first_guard) ||
            !ReadValue(process, image_base + 0xB6BCE8, second_guard) ||
            first_guard != 1 || second_guard != 1)
            return Fail(failure, "lua_linker", guard_predicate, 0,
                        image_base + 0xB6BCD8, first_guard, second_guard,
                        GetLastError());
        return true;
    };

    if (module_layout == LuaModuleRegistrarLayout::CanonicalPrefix &&
        module_count == 15 &&
        restricted_layout == LuaRestrictedRegistrarLayout::Canonical) {
        if (!verify_linker_runtime("partial_linker_dependency",
                                   "partial_linker_guards"))
            return false;
        if (!CallLuaNamedRegistrar(
                pid, process, image_base, "linker", 0x9C940, 0x599F0,
                15, failure, rollback_safe,
                "partial_linker_full_register"))
            return false;
        module_vector = {};
        restricted_vector = {};
        module_records = {};
        module_count = 0;
        if (!ReadLuaModuleRegistrarState(
                process, image_base, module_vector, module_records,
                module_count, module_layout, failure,
                "module_after_partial_linker") ||
            !ReadLuaRestrictedRegistrarState(
                process, image_base, restricted_vector,
                restricted_layout, failure,
                "restricted_after_partial_linker") ||
            module_layout != LuaModuleRegistrarLayout::CanonicalPrefix ||
            module_count != 16 ||
            restricted_layout !=
                LuaRestrictedRegistrarLayout::Canonical)
            return Fail(failure, "lua_linker",
                        "partial_linker_exact_append", 0,
                        image_base + 0xB6B308, module_count, 16,
                        GetLastError());
    }

    const bool resumable_chain =
        (module_layout == LuaModuleRegistrarLayout::Empty &&
         restricted_layout == LuaRestrictedRegistrarLayout::Empty) ||
        (module_layout == LuaModuleRegistrarLayout::CanonicalPrefix &&
         ((module_count <= 15 &&
           restricted_layout == LuaRestrictedRegistrarLayout::Empty) ||
          (module_count >= 16 &&
           restricted_layout ==
               LuaRestrictedRegistrarLayout::Canonical))) ||
        (module_layout == LuaModuleRegistrarLayout::Canonical &&
         restricted_layout == LuaRestrictedRegistrarLayout::Canonical);
    if (resumable_chain &&
        !EnsureCompleteLuaRegistrarChain(pid, process, image_base, failure,
                                         rollback_safe))
        return false;

    module_vector = {};
    restricted_vector = {};
    module_records = {};
    module_count = 0;
    if (!ReadLuaModuleRegistrarState(
            process, image_base, module_vector, module_records,
            module_count, module_layout, failure,
            "module_pair_prestate") ||
        !ReadLuaRestrictedRegistrarState(
            process, image_base, restricted_vector, restricted_layout,
            failure, "restricted_pair_prestate"))
        return false;
    const bool initialize =
        module_layout == LuaModuleRegistrarLayout::MissingLinker &&
        restricted_layout == LuaRestrictedRegistrarLayout::Empty;
    const bool repair =
        (module_layout == LuaModuleRegistrarLayout::MissingLinker ||
         module_layout == LuaModuleRegistrarLayout::LinkerAtTail) &&
        restricted_layout == LuaRestrictedRegistrarLayout::Canonical;
    const bool canonical =
        module_layout == LuaModuleRegistrarLayout::Canonical &&
        restricted_layout == LuaRestrictedRegistrarLayout::Canonical;
    if (!initialize && !repair && !canonical)
        return Fail(
            failure, "lua_linker", "registrar_pair_prestate", 0,
            image_base + 0xB6B308,
            static_cast<std::uint64_t>(module_layout) |
                (static_cast<std::uint64_t>(restricted_layout) << 8),
            static_cast<std::uint64_t>(
                LuaModuleRegistrarLayout::Canonical) |
                (static_cast<std::uint64_t>(
                     LuaRestrictedRegistrarLayout::Canonical)
                 << 8));
    if (initialize) {
        if (!PrepareLuaLinkerOriginalBuilders(
                pid, process, image_base, failure, rollback_safe))
            return false;
        if (!InvokeLuaLinkerCarrier(pid, image_base + 0x9CD90, failure,
                                    "initializer", 0, rollback_safe))
            return false;
        module_vector = {};
        restricted_vector = {};
        module_records = {};
        module_count = 0;
        if (!ReadLuaModuleRegistrarState(
                process, image_base, module_vector, module_records,
                module_count, module_layout, failure,
                "module_after_initializer") ||
            !ReadLuaRestrictedRegistrarState(
                process, image_base, restricted_vector,
                restricted_layout, failure,
                "restricted_after_initializer"))
            return false;
        if (module_layout != LuaModuleRegistrarLayout::LinkerAtTail ||
            restricted_layout !=
                LuaRestrictedRegistrarLayout::Canonical)
            return Fail(
                failure, "lua_linker", "initializer_exact_append", 0,
                image_base + 0xB6B308,
                static_cast<std::uint64_t>(module_layout) |
                    (static_cast<std::uint64_t>(restricted_layout) << 8),
                static_cast<std::uint64_t>(
                    LuaModuleRegistrarLayout::LinkerAtTail) |
                    (static_cast<std::uint64_t>(
                         LuaRestrictedRegistrarLayout::Canonical)
                     << 8));
    }
    if (module_layout == LuaModuleRegistrarLayout::MissingLinker &&
        restricted_layout == LuaRestrictedRegistrarLayout::Canonical) {
        if (!verify_linker_runtime("missing_linker_dependency",
                                   "missing_linker_guards"))
            return false;
        if (!CallLuaNamedRegistrar(
                pid, process, image_base, "linker", 0x9C940, 0x599F0,
                15, failure, rollback_safe,
                "missing_linker_full_register"))
            return false;
        module_vector = {};
        restricted_vector = {};
        module_records = {};
        module_count = 0;
        if (!ReadLuaModuleRegistrarState(
                process, image_base, module_vector, module_records,
                module_count, module_layout, failure,
                "module_after_missing_linker") ||
            !ReadLuaRestrictedRegistrarState(
                process, image_base, restricted_vector,
                restricted_layout, failure,
                "restricted_after_missing_linker") ||
            module_layout != LuaModuleRegistrarLayout::LinkerAtTail ||
            restricted_layout !=
                LuaRestrictedRegistrarLayout::Canonical)
            return Fail(failure, "lua_linker",
                        "missing_linker_exact_append", 0,
                        image_base + 0xB6B308,
                        static_cast<std::uint64_t>(module_layout),
                        static_cast<std::uint64_t>(
                            LuaModuleRegistrarLayout::LinkerAtTail),
                        GetLastError());
    }
    if (module_layout == LuaModuleRegistrarLayout::LinkerAtTail &&
        !RotateLuaRegistrarLinkerIntoCanonicalOrder(
            process, image_base, module_vector, module_records, failure,
            rollback_safe))
        return false;
    module_vector = {};
    restricted_vector = {};
    module_records = {};
    module_count = 0;
    if (!ReadLuaModuleRegistrarState(
            process, image_base, module_vector, module_records,
            module_count, module_layout, failure, "module_poststate") ||
        !ReadLuaRestrictedRegistrarState(
            process, image_base, restricted_vector, restricted_layout,
            failure, "restricted_poststate"))
        return false;
    if (module_layout != LuaModuleRegistrarLayout::Canonical ||
        restricted_layout != LuaRestrictedRegistrarLayout::Canonical)
        return Fail(
            failure, "lua_linker", "registrar_pair_poststate", 0,
            image_base + 0xB6B308,
            static_cast<std::uint64_t>(module_layout) |
                (static_cast<std::uint64_t>(restricted_layout) << 8),
            static_cast<std::uint64_t>(
                LuaModuleRegistrarLayout::Canonical) |
                (static_cast<std::uint64_t>(
                     LuaRestrictedRegistrarLayout::Canonical)
                 << 8));
    return verify_linker_runtime("dependency_executable",
                                 "registry_guards");
}

struct RemoteLuaStateName {
    std::array<char, 16> storage{};
    std::uint64_t size = 0;
    std::uint64_t capacity = 15;
};

static_assert(sizeof(RemoteLuaStateName) == 32);

bool QueryLuaNamedState(HANDLE process, std::uint64_t image_base,
                        const char* name, std::size_t index,
                        std::uint64_t& named_state, Failure& failure,
                        const char* stage) {
    constexpr std::array<std::uint8_t, 8> lookup_prefix{{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC}};
    const std::uint64_t lookup = image_base + 0x56F80;
    if (!ExecutableRange(process, lookup, lookup_prefix.size()) ||
        !VerifyBytes(process, lookup, lookup_prefix.data(),
                     lookup_prefix.size(), failure, stage,
                     "lookup_prefix", index))
        return false;

    RemoteLuaStateName value{};
    value.size = std::strlen(name);
    if (value.size >= value.storage.size())
        return Fail(failure, stage, "name_sso", index, value.size,
                    value.storage.size() - 1);
    std::memcpy(value.storage.data(), name, value.size);

    void* remote_value = VirtualAllocEx(
        process, nullptr, sizeof(value), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!remote_value)
        return Fail(failure, stage, "lookup_allocate", index, 0, 0,
                    sizeof(value), GetLastError());
    const std::uint64_t remote_address =
        reinterpret_cast<std::uint64_t>(remote_value);
    if (!WriteExact(process, remote_address, &value, sizeof(value))) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_value, 0, MEM_RELEASE);
        return Fail(failure, stage, "lookup_write", index,
                    remote_address, 0, sizeof(value), error);
    }

    named_state = 0;
    const bool called = RemoteCall3(
        process, lookup, remote_address, 0, 0, named_state, failure,
        stage, index);
    const bool quiescent = RemoteCallsRollbackSafe();
    const bool freed = quiescent &&
        VirtualFreeEx(process, remote_value, 0, MEM_RELEASE) != FALSE;
    if (!quiescent)
        return false;
    if (!called)
        return false;
    if (!freed)
        return Fail(failure, stage, "lookup_free", index,
                    remote_address, 0, 1, GetLastError());
    return true;
}

bool CountLuaRegisteredState(HANDLE process, std::uint64_t image_base,
                             std::uint64_t lua_state,
                             std::size_t& occurrences, Failure& failure,
                             const char* stage, std::size_t index) {
    constexpr std::uint64_t vector_rva = 0xB6B338;
    constexpr std::size_t maximum_states = 4096;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t capacity = 0;
    if (!lua_state ||
        !ReadValue(process, image_base + vector_rva, begin) ||
        !ReadValue(process, image_base + vector_rva + 8, end) ||
        !ReadValue(process, image_base + vector_rva + 16, capacity))
        return Fail(failure, stage, "vector_read", index,
                    image_base + vector_rva, begin, end, GetLastError());
    if ((begin || end || capacity) &&
        (!begin || end < begin || capacity < end ||
         ((begin | end | capacity) & 7u) != 0 ||
         ((end - begin) % sizeof(std::uint64_t)) != 0 ||
         (end - begin) / sizeof(std::uint64_t) > maximum_states ||
         (end != begin && !ReadableRange(process, begin, end - begin))))
        return Fail(failure, stage, "vector_shape", index,
                    image_base + vector_rva, begin, end, GetLastError());

    occurrences = 0;
    for (std::uint64_t cursor = begin; cursor && cursor != end;
         cursor += sizeof(std::uint64_t)) {
        std::uint64_t entry = 0;
        if (!ReadValue(process, cursor, entry))
            return Fail(failure, stage, "vector_entry", index, cursor,
                        0, lua_state, GetLastError());
        if (entry == lua_state)
            ++occurrences;
    }
    return true;
}

bool VerifyLuaRegistrationIdentity(HANDLE process,
                                   std::uint64_t image_base,
                                   std::uint64_t lua_state,
                                   const char* name, std::size_t index,
                                   Failure& failure, const char* stage) {
    std::size_t occurrences = 0;
    std::uint64_t named_state = 0;
    if (!CountLuaRegisteredState(process, image_base, lua_state,
                                 occurrences, failure, stage, index) ||
        !QueryLuaNamedState(process, image_base, name, index,
                            named_state, failure, stage))
        return false;
    if (occurrences != 1 || named_state != lua_state)
        return Fail(failure, stage, "identity_exact", index,
                    image_base + 0xB69500, named_state, lua_state,
                    occurrences == 1 ? ERROR_SUCCESS : ERROR_INVALID_DATA);
    return true;
}

bool EnsureLuaRegistration(HANDLE process, std::uint64_t image_base,
                           LuaBootstrapState& state, Failure& failure) {
    auto ensure_named_state = [&](std::uint64_t lua_state, const char* name,
                                  std::size_t index,
                                  bool& seeded) -> bool {
        std::size_t occurrences = 0;
        std::uint64_t named_state = 0;
        if (!CountLuaRegisteredState(
                process, image_base, lua_state, occurrences, failure,
                "lua_registration", index) ||
            !QueryLuaNamedState(process, image_base, name, index,
                                named_state, failure,
                                "lua_registration"))
            return false;
        if (occurrences > 1)
            return Fail(failure, "lua_registration", "state_unique",
                        index, image_base + 0xB6B338, occurrences, 1,
                        ERROR_INVALID_DATA);
        if (occurrences == 1 && named_state == lua_state) {
            seeded = false;
            return true;
        }
        const bool repair_vector =
            occurrences == 0 && named_state == lua_state;
        const bool repair_name =
            occurrences == 1 && named_state == 0;
        const bool fresh = occurrences == 0 && named_state == 0;
        if (!fresh && !repair_vector && !repair_name)
            return Fail(failure, "lua_registration",
                        "vector_name_agreement", index,
                        image_base + 0xB69500, named_state,
                        occurrences == 0 ? 0 : lua_state,
                        ERROR_INVALID_DATA);

        RemoteLuaStateName value{};
        value.size = std::strlen(name);
        if (value.size >= value.storage.size())
            return Fail(failure, "lua_registration", "name_sso", index,
                        value.size, value.storage.size() - 1);
        std::memcpy(value.storage.data(), name, value.size);
        void* remote_value = VirtualAllocEx(
            process, nullptr, sizeof(value), MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!remote_value)
            return Fail(failure, "lua_registration", "remote_allocate",
                        index,
                        0, 0, sizeof(value), GetLastError());
        const std::uint64_t remote_address =
            reinterpret_cast<std::uint64_t>(remote_value);
        if (!WriteExact(process, remote_address, &value, sizeof(value))) {
            const DWORD error = GetLastError();
            VirtualFreeEx(process, remote_value, 0, MEM_RELEASE);
            return Fail(failure, "lua_registration", "remote_write", index,
                        remote_address, 0, sizeof(value), error);
        }
        std::uint64_t result = 0;
        seeded = fresh;
        const bool called = RemoteCall3(
            process, image_base + 0x573D0, lua_state, remote_address, 1,
            result, failure, "lua_registration_call", index);
        const bool quiescent = RemoteCallsRollbackSafe();
        const bool freed = quiescent &&
            VirtualFreeEx(process, remote_value, 0, MEM_RELEASE) != FALSE;
        if (!quiescent)
            return false;
        if (!called)
            return false;
        if (!freed)
            return Fail(failure, "lua_registration", "remote_free", index,
                        remote_address, 0, 1, GetLastError());
        return VerifyLuaRegistrationIdentity(
            process, image_base, lua_state, name, index, failure,
            "lua_registration_post");
    };
    if (!ensure_named_state(state.menu_lua_state, "menu", 2,
                            state.menu_registration_seeded))
        return false;
    if (state.deferred)
        return true;
    if (!state.realm ||
        !ReadValue(process, state.realm + 8, state.lua_state) ||
        !state.lua_state)
        return Fail(failure, "lua_registration", "client_state_present", 0,
                    state.realm, state.lua_state, 1, GetLastError());
    return ensure_named_state(state.lua_state, "client", 0,
                              state.registration_seeded);
}

bool VerifyLuaLinkerReplay(HANDLE process, std::uint64_t image_base,
                           const LuaBootstrapState& state,
                           Failure& failure) {
    const std::uint64_t expected_original = state.original;
    const std::uint64_t detour = image_base + 0x31DE30;
    if (state.deferred) {
        if (state.original != expected_original ||
            state.linker_replay_allocation ||
            state.linker_replay_context || state.linker_replay_thunk ||
            state.linker_replay_previous_saved ||
            state.linker_replay_entry_count ||
            state.linker_replay_published ||
            state.linker_replay_saved_changed)
            return Fail(failure, "lua_linker_replay_post",
                        "deferred_unpublished", 0,
                        state.linker_replay_allocation,
                        state.linker_replay_thunk, 0,
                        ERROR_INVALID_DATA);

        std::uint64_t realm = 0;
        std::uint64_t saved = 0;
        if (!ReadValue(process, image_base + 0x7EA870, realm) ||
            !ReadValue(process, image_base + 0x823868, saved))
            return Fail(failure, "lua_linker_replay_post",
                        "deferred_slots_read", 0,
                        image_base + 0x7EA870, realm, saved,
                        GetLastError());
        if (!realm) {
            if (saved != 0)
                return Fail(failure, "lua_linker_replay_post",
                            "deferred_saved_zero", 0,
                            image_base + 0x823868, saved, 0);
            for (std::size_t index = 0;
                 index != state.realm_vtable_count;
                  ++index) {
                const std::uint64_t slot = state.realm_vtables[index] +
                    111 * sizeof(std::uint64_t);
                std::uint64_t live = 0;
                if (!ReadValue(process, slot, live) ||
                    live != expected_original)
                    return Fail(failure, "lua_linker_replay_post",
                                "deferred_vtable_clean", index, slot, live,
                                expected_original, GetLastError());
            }
            return true;
        }

        std::uint64_t vtable = 0;
        std::uint64_t live = 0;
        if (saved != expected_original ||
            !ReadValue(process, realm, vtable) ||
            !LuaRealmVtableAccepted(state, vtable) ||
            !ReadValue(process, vtable + 111 * sizeof(std::uint64_t), live) ||
            live != detour)
            return Fail(failure, "lua_linker_replay_post",
                        "deferred_natural_topology", 0,
                        vtable + 111 * sizeof(std::uint64_t), live, detour,
                        GetLastError());
        return VerifyBytes(process, expected_original,
                           kLuaRunStringOriginalPrefix,
                           sizeof(kLuaRunStringOriginalPrefix), failure,
                           "lua_linker_replay_post",
                           "deferred_original_prefix", 0) &&
               VerifyBytes(process, detour, kDetour31de30,
                           kDetourPrefixSize, failure,
                           "lua_linker_replay_post",
                           "deferred_detour_prefix", 0);
    }

    if (!state.linker_replay_published ||
        !state.linker_replay_allocation ||
        state.linker_replay_context != state.linker_replay_allocation ||
        state.linker_replay_thunk !=
            state.linker_replay_allocation + 0x1000 ||
        !ReadableRange(process, state.linker_replay_context,
                       sizeof(LuaLinkerReplayContext)) ||
        !ExecutableRange(process, state.linker_replay_thunk,
                         kLuaLinkerReplayThunk.size()))
        return Fail(failure, "lua_linker_replay_post", "allocation", 0,
                    state.linker_replay_allocation,
                    state.linker_replay_thunk, 1, GetLastError());
    LuaLinkerReplayContext context{};
    std::uint64_t embedded_context = 0;
    std::uint64_t saved = 0;
    if (!ReadExact(process, state.linker_replay_context, &context,
                   sizeof(context)) ||
        !ReadValue(process, state.linker_replay_thunk + 0x48,
                   embedded_context) ||
        !ReadValue(process, image_base + 0x823868, saved))
        return Fail(failure, "lua_linker_replay_post", "read", 0,
                    state.linker_replay_context, 0, 1, GetLastError());
    const bool coherent_guard =
        (context.guard == 0 && context.status != 1 &&
         context.status != 2) ||
        (context.guard == 1 && context.status == 1) ||
        (context.guard == 2 && context.status == 2);
    const std::uint64_t context_original = context.original;
    if (!coherent_guard || embedded_context != state.linker_replay_context ||
        context.realm_slot != image_base + 0x7EA870 ||
        context_original != state.original ||
        context.build_environment != image_base + 0x5A0A0 ||
        context.is_type != image_base + 0x55B50 ||
        context.pop != image_base + 0x55E80 ||
        context.registrar != image_base + 0x9C940 ||
        context.ownership_global != image_base + 0x7CE8C0 ||
        context.ownership_lookup != image_base + 0x618D0 ||
        context.ownership_insert != image_base + 0x608E0 ||
        context.saved_slot != image_base + 0x823868 ||
        std::memcmp(context.linker_name.data(), "linker", 7) != 0 ||
        (saved != state.linker_replay_thunk &&
         saved != expected_original))
        return Fail(failure, "lua_linker_replay_post", "context_exact", 0,
                    state.linker_replay_context, context.status,
                    context.guard, GetLastError());
    if (context.guard == 2 && saved != expected_original)
        return Fail(failure, "lua_linker_replay_post", "self_restore", 0,
                    image_base + 0x823868, saved, expected_original);
    for (std::size_t index = 0;
         index != state.linker_replay_entry_count; ++index) {
        std::uint64_t live = 0;
        if (!ReadValue(process, state.linker_replay_entry_slots[index],
                       live) ||
            live != image_base + 0x31DE30)
            return Fail(failure, "lua_linker_replay_post",
                        "deferred_entry", index,
                        state.linker_replay_entry_slots[index], live,
                        image_base + 0x31DE30, GetLastError());
    }
    return true;
}

bool VerifyLuaBootstrap(HANDLE process, std::uint64_t image_base,
                        const LuaBootstrapState& state, Failure& failure) {
    std::uint64_t realm = 0;
    std::uint64_t vtable = 0;
    std::uint64_t live = 0;
    std::uint64_t saved = 0;
    const std::uint64_t detour = image_base + 0x31DE30;
    std::uint64_t menu_lua_state = 0;
    if (!state.menu_realm || !state.menu_lua_state ||
        !ReadValue(process, state.menu_realm + 8, menu_lua_state) ||
        menu_lua_state != state.menu_lua_state)
        return Fail(failure, "lua_post", "menu_realm_identity", 2,
                    state.menu_realm + 8, menu_lua_state,
                    state.menu_lua_state, GetLastError());
    if (!VerifyLuaRegistrationIdentity(
            process, image_base, state.menu_lua_state, "menu", 2, failure,
            "lua_menu_post") ||
        !VerifyLuaLinkerReplay(process, image_base, state, failure))
        return false;
    const auto saved_valid = [&](std::uint64_t value) {
        return value == state.original ||
               value == state.linker_replay_thunk;
    };
    if (state.deferred) {
        if (!ReadValue(process, image_base + 0x7EA870, realm) ||
            !ReadValue(process, image_base + 0x823868, saved))
            return Fail(failure, "lua_post", "deferred_slots_read", 0,
                        image_base + 0x7EA870, realm, saved,
                        GetLastError());
        if (!realm) {
            std::uint64_t named_client = 0;
            if (saved != 0 ||
                !QueryLuaNamedState(process, image_base, "client", 0,
                                    named_client, failure,
                                    "lua_client_pending_post"))
                return saved != 0
                    ? Fail(failure, "lua_post", "deferred_saved_zero", 0,
                           image_base + 0x823868, saved, 0)
                    : false;
            if (named_client != 0)
                return Fail(failure, "lua_post",
                            "deferred_client_name_absent", 0,
                            image_base + 0xB69500, named_client, 0,
                            ERROR_INVALID_DATA);
            return true;
        }

        std::uint64_t client_lua_state = 0;
        std::size_t occurrences = 0;
        std::uint64_t named_client = 0;
        if (saved != state.original ||
            !ReadValue(process, realm, vtable) ||
            !LuaRealmVtableAccepted(state, vtable) ||
            !ReadValue(process, vtable + 111 * 8, live) || live != detour ||
            !ReadValue(process, realm + 8, client_lua_state) ||
            !client_lua_state || client_lua_state == state.menu_lua_state ||
            !CountLuaRegisteredState(
                process, image_base, client_lua_state, occurrences, failure,
                "lua_client_deferred_post", 0) ||
            !QueryLuaNamedState(process, image_base, "client", 0,
                                named_client, failure,
                                "lua_client_deferred_post"))
            return Fail(failure, "lua_post", "deferred_consistent", 0,
                        vtable + 111 * 8, live, detour, GetLastError());
        const bool registration_pending =
            occurrences == 0 && named_client == 0;
        const bool registration_ready =
            occurrences == 1 && named_client == client_lua_state;
        if (!registration_pending && !registration_ready)
            return Fail(failure, "lua_client_deferred_post",
                        "registration_agreement", 0,
                        image_base + 0xB69500, named_client,
                        client_lua_state, ERROR_INVALID_DATA);
        return true;
    }
    std::uint64_t client_lua_state = 0;
    if (!ReadValue(process, image_base + 0x7EA870, realm) ||
        realm != state.realm || !ReadValue(process, realm + 8,
                                           client_lua_state) ||
        client_lua_state != state.lua_state ||
        client_lua_state == state.menu_lua_state ||
        !ReadValue(process, realm, vtable) ||
        !LuaRealmVtableAccepted(state, vtable) ||
        !ReadValue(process, vtable + 111 * 8, live) ||
        !ReadValue(process, image_base + 0x823868, saved) ||
        live != detour || !saved_valid(saved) || !state.original ||
        !ExecutableRange(process, state.original) ||
        !VerifyBytes(process, state.original, kLuaRunStringOriginalPrefix,
                     sizeof(kLuaRunStringOriginalPrefix), failure,
                     "lua_post", "original_prefix", 0) ||
        !VerifyBytes(process, detour, kDetour31de30, kDetourPrefixSize,
                     failure, "lua_post", "detour_prefix", 0))
        return Fail(failure, "lua_post", "bootstrap_exact", 0,
                    vtable + 111 * 8, live, detour, GetLastError());
    return VerifyLuaRegistrationIdentity(
        process, image_base, state.lua_state, "client", 0, failure,
        "lua_client_post");
}

bool RollbackLuaBootstrap(HANDLE process, std::uint64_t image_base,
                           LuaBootstrapState& state) {
    bool ok = true;
    auto unregister_seeded = [&](std::uint64_t lua_state,
                                 const char* name, std::size_t index,
                                 bool& seeded) -> bool {
        if (!seeded || !lua_state)
            return true;
        Failure ignored{};
        std::uint64_t result = 0;
        const bool called = RemoteCall3(
            process, image_base + 0x56C50, lua_state, 0, 0, result, ignored,
            "lua_registration_rollback", index);
        if (!called)
            return false;
        std::size_t occurrences = 0;
        std::uint64_t named_state = UINT64_MAX;
        if (!CountLuaRegisteredState(
                process, image_base, lua_state, occurrences, ignored,
                "lua_registration_rollback_post", index) ||
            !QueryLuaNamedState(
                process, image_base, name, index, named_state, ignored,
                "lua_registration_rollback_post") ||
            occurrences != 0 || named_state != 0)
            return false;
        seeded = false;
        return true;
    };

    if (!unregister_seeded(state.lua_state, "client", 0,
                           state.registration_seeded)) {
        if (!RemoteCallsRollbackSafe())
            return false;
        ok = false;
    }
    if (!unregister_seeded(state.menu_lua_state, "menu", 2,
                           state.menu_registration_seeded)) {
        if (!RemoteCallsRollbackSafe())
            return false;
        ok = false;
    }
    bool detached = true;
    if (state.entry_changed)
        detached = WriteProtectedQword(process, state.entry_slot,
                                       state.original);
    if (!detached)
        ok = false;
    bool replay_entries_detached = true;
    for (std::size_t index = state.linker_replay_entry_count;
         index != 0; --index) {
        const std::size_t entry = index - 1;
        if (!WriteProtectedQword(
                process, state.linker_replay_entry_slots[entry],
                state.linker_replay_entry_previous[entry]))
            replay_entries_detached = false;
    }
    if (!replay_entries_detached)
        ok = false;
    if (detached && replay_entries_detached &&
        state.linker_replay_saved_changed &&
        !WriteProtectedQword(process, image_base + 0x823868,
                             state.linker_replay_previous_saved))
        ok = false;
    if (detached && state.saved_changed &&
        !WriteProtectedQword(process, image_base + 0x823868,
                             state.previous_saved))
        ok = false;
    if (detached && state.realm_changed &&
        !WriteProtectedQword(process, image_base + 0x7EA870,
                             state.previous_realm_slot))
        ok = false;
    if (state.linker_replay_allocation &&
        !state.linker_replay_published &&
        !VirtualFreeEx(process,
                       reinterpret_cast<void*>(
                           state.linker_replay_allocation),
                       0, MEM_RELEASE))
        ok = false;
    return ok;
}

bool AbsoluteJump(const std::uint8_t* code, std::size_t size,
                   std::uint64_t& target) {
    if (!code || size < 14 || code[0] != 0xFF || code[1] != 0x25 ||
        code[2] != 0 || code[3] != 0 || code[4] != 0 || code[5] != 0)
        return false;
    std::memcpy(&target, code + 6, sizeof(target));
    return target != 0;
}

bool RelativeJumpTarget(const std::array<std::uint8_t, kTargetPrefixSize>&
                            patch,
                        std::uint64_t source, std::uint64_t& target) {
    if (patch[0] != 0xE9 || source >
            static_cast<std::uint64_t>(INT64_MAX) - patch.size())
        return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, patch.data() + 1, sizeof(displacement));
    const std::int64_t resolved =
        static_cast<std::int64_t>(source + patch.size()) + displacement;
    if (resolved <= 0)
        return false;
    target = static_cast<std::uint64_t>(resolved);
    return true;
}

bool VerifyDynamicMinHook(HANDLE process, DynamicMinHookState& state,
                          Failure& failure, std::size_t index,
                          const char* stage) {
    std::uint64_t saved = 0;
    if (!state.saved_slot ||
        !ReadValue(process, state.saved_slot, saved) || !saved)
        return Fail(failure, stage, "trampoline_nonzero", index,
                    state.saved_slot, saved, 1, GetLastError());
    if (state.saved && state.saved != saved)
        return Fail(failure, stage, "trampoline_stable", index,
                    state.saved_slot, saved, state.saved);
    state.saved = saved;
    if ((saved & (kMinHookBlockSize - 1)) != 0)
        return Fail(failure, stage, "trampoline_alignment", index, saved,
                    saved & (kMinHookBlockSize - 1), 0);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(saved),
                       &memory, sizeof(memory)) != sizeof(memory))
        return Fail(failure, stage, "trampoline_query", index, saved, 0, 0,
                    GetLastError());
    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
        (memory.Protect & 0xFFu) != PAGE_EXECUTE_READWRITE ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !memory.AllocationBase)
        return Fail(failure, stage, "trampoline_memory_exact", index, saved,
                    memory.Protect, PAGE_EXECUTE_READWRITE);
    std::array<std::uint8_t, kMinHookBlockSize> code{};
    if (!ExecutableRange(process, saved, code.size()) ||
        !ReadExact(process, saved, code.data(), code.size()))
        return Fail(failure, stage, "trampoline_read", index, saved, 0, 0,
                    GetLastError());
    if (!state.detour ||
        !ExecutableRange(process, state.detour, state.detour_prefix.size()))
        return Fail(failure, stage, "detour_executable", index,
                    state.detour, 0, 1, GetLastError());
    std::array<std::uint8_t, kDetourPrefixSize> detour{};
    if (!ReadExact(process, state.detour, detour.data(), detour.size()) ||
        detour != state.detour_prefix)
        return Fail(failure, stage, "detour_prefix", index, state.detour,
                    PrefixValue(detour.data(), detour.size()),
                    PrefixValue(state.detour_prefix.data(),
                                state.detour_prefix.size()),
                    GetLastError());

    std::uint64_t slotted_target = 0;
    if (state.has_target_slot) {
        if (!state.target_slot ||
            !ReadValue(process, state.target_slot, slotted_target) ||
            !slotted_target)
            return Fail(failure, stage, "target_slot_nonzero", index,
                        state.target_slot, slotted_target, 1,
                        GetLastError());
        if (state.target && state.target != slotted_target)
            return Fail(failure, stage, "target_slot_stable", index,
                        state.target_slot, slotted_target, state.target);
        state.target = slotted_target;
    }

    std::size_t matches = 0;
    std::uint64_t matched_target = 0;
    std::uint64_t matched_relay = 0;
    for (std::size_t offset = kTargetPrefixSize;
         offset + 28 <= code.size(); ++offset) {
        std::uint64_t resume = 0;
        std::uint64_t relay_target = 0;
        if (!AbsoluteJump(code.data() + offset, code.size() - offset,
                          resume) ||
            !AbsoluteJump(code.data() + offset + 14,
                          code.size() - offset - 14, relay_target) ||
            relay_target != state.detour)
            continue;
        const std::uint64_t relay = saved + offset + 14;
        const std::size_t first_span = state.target ? 0 : kTargetPrefixSize;
        const std::size_t last_span = state.target ? 0 : 0x20;
        for (std::size_t span = first_span; span <= last_span; ++span) {
            if (!state.target && resume < span)
                continue;
            const std::uint64_t candidate =
                state.target ? state.target : resume - span;
            if (resume < candidate + kTargetPrefixSize ||
                resume > candidate + 0x20)
                continue;
            std::array<std::uint8_t, kTargetPrefixSize> patch{};
            std::uint64_t observed_relay = 0;
            if (!ExecutableRange(process, candidate, patch.size()) ||
                !ReadExact(process, candidate, patch.data(), patch.size()) ||
                !RelativeJumpTarget(patch, candidate, observed_relay) ||
                observed_relay != relay)
                continue;
            ++matches;
            matched_target = candidate;
            matched_relay = relay;
            if (state.target)
                break;
        }
    }
    if (matches != 1)
        return Fail(failure, stage, "unique_e9_detour", index, saved,
                    matches, 1);
    if (state.has_target_slot && matched_target != slotted_target)
        return Fail(failure, stage, "target_slot_exact", index,
                    state.target_slot, slotted_target, matched_target);
    state.target = matched_target;
    state.relay = matched_relay;
    std::copy_n(code.begin(), state.target_prefix.size(),
                state.target_prefix.begin());
    state.prefix_known = true;
    return true;
}

bool RollbackDynamicMinHook(HANDLE process, std::uint64_t image_base,
                            DynamicMinHookState& state,
                            std::size_t index) {
    if (!state.created)
        return true;
    Failure ignored{};
    std::uint64_t live_target = state.target;
    std::uint64_t live_saved = state.saved;
    if (state.has_target_slot) {
        if (!ReadValue(process, state.target_slot, live_target) ||
            !ReadValue(process, state.saved_slot, live_saved))
            return false;
        state.target = live_target;
        state.saved = live_saved;
    } else if (!ReadValue(process, state.saved_slot, live_saved)) {
        return false;
    } else {
        state.saved = live_saved;
    }
    if (!live_target && !live_saved) {
        state.created = false;
        return true;
    }
    if ((!state.target || !state.prefix_known) && live_saved) {
        if (!VerifyDynamicMinHook(process, state, ignored, index,
                                  "rollback_dynamic_recover"))
            return false;
        live_target = state.target;
        live_saved = state.saved;
    }
    if (state.target && !state.prefix_known && !live_saved) {
        if (!ReadExact(process, state.target, state.target_prefix.data(),
                       state.target_prefix.size()) ||
            state.target_prefix[0] == 0xE9)
            return false;
        state.prefix_known = true;
    }
    if (!state.target)
        return false;

    if (state.cleanup_rva) {
        if (!ExecutableRange(process, image_base + state.cleanup_rva,
                             state.cleanup_prefix.size()) ||
            !VerifyBytes(process, image_base + state.cleanup_rva,
                         state.cleanup_prefix.data(),
                         state.cleanup_prefix.size(), ignored,
                         "rollback_dynamic", "cleanup_prefix", index))
            return false;
        std::uint64_t result = UINT64_MAX;
        if (!RemoteCall3(process, image_base + state.cleanup_rva, 0, 0, 0,
                         result, ignored, "rollback_dynamic_cleanup", index))
            return false;
    } else {
        std::uint64_t result = UINT64_MAX;
        if (!RemoteCall3(process, image_base + kEnableHookRva,
                         state.target, 0, 0, result, ignored,
                         "rollback_dynamic_disable", index))
            return false;
        const std::uint32_t disable_status =
            static_cast<std::uint32_t>(result);
        if (disable_status != 0 && disable_status != 6)
            return false;
        result = UINT64_MAX;
        if (!RemoteCall3(process, image_base + kRemoveHookRva,
                         state.target, 0, 0, result, ignored,
                         "rollback_dynamic_remove", index))
            return false;
        const std::uint32_t remove_status =
            static_cast<std::uint32_t>(result);
        if (remove_status != 0 && remove_status != 4)
            return false;
        if (!WriteProtectedQword(process, state.saved_slot, 0))
            return false;
    }

    std::array<std::uint8_t, kTargetPrefixSize> restored{};
    if (!ReadExact(process, state.target, restored.data(), restored.size()) ||
        (state.prefix_known ? restored != state.target_prefix
                            : restored[0] == 0xE9))
        return false;
    std::uint64_t target_after = 0;
    std::uint64_t saved_after = 0;
    if ((state.has_target_slot &&
         (!ReadValue(process, state.target_slot, target_after) ||
          target_after != 0)) ||
        !ReadValue(process, state.saved_slot, saved_after) ||
        saved_after != 0)
        return false;
    state.created = false;
    return true;
}

bool RollbackDynamicMinHooks(
    HANDLE process, std::uint64_t image_base,
    std::array<DynamicMinHookState, 2>& dynamic_hooks,
    DynamicMinHookState& mouse_hook) {
    bool ok = RollbackDynamicMinHook(process, image_base, mouse_hook,
                                     dynamic_hooks.size());
    if (!RemoteCallsRollbackSafe())
        return false;
    for (std::size_t reverse = dynamic_hooks.size(); reverse != 0;
         --reverse) {
        if (!RollbackDynamicMinHook(process, image_base,
                                    dynamic_hooks[reverse - 1],
                                    reverse - 1))
            ok = false;
        if (!RemoteCallsRollbackSafe())
            return false;
    }
    return ok;
}

struct MinState {
    std::uint64_t target = 0;
    std::uint64_t detour = 0;
    std::uint64_t saved_slot = 0;
    std::array<std::uint8_t, kTargetPrefixSize> target_prefix{};
    std::array<std::uint8_t, kDetourPrefixSize> detour_prefix{};
    bool installed = false;
    bool created_present = false;
    bool created = false;
    bool enabled = false;
};

bool VerifyMinHook(HANDLE process, const ModuleInfo& module,
                   const MinState& state,
                   std::uint64_t& common_allocation, Failure& failure,
                   std::size_t index, const char* stage,
                   bool target_enabled = true) {
    std::uint64_t trampoline = 0;
    if (!ReadValue(process, state.saved_slot, trampoline) || !trampoline)
        return Fail(failure, stage, "trampoline_nonzero", index,
                    state.saved_slot, trampoline, 1, GetLastError());
    if ((trampoline & (kMinHookBlockSize - 1)) != 0)
        return Fail(failure, stage, "trampoline_alignment", index,
                    trampoline, trampoline & (kMinHookBlockSize - 1), 0);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(trampoline),
                       &memory, sizeof(memory)) != sizeof(memory))
        return Fail(failure, stage, "trampoline_query", index, trampoline,
                    0, 0, GetLastError());
    const auto allocation =
        reinterpret_cast<std::uint64_t>(memory.AllocationBase);
    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
        (memory.Protect & 0xFFu) != PAGE_EXECUTE_READWRITE ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 || !allocation)
        return Fail(failure, stage, "trampoline_memory_exact", index,
                    trampoline, memory.Protect, PAGE_EXECUTE_READWRITE);
    if (!common_allocation)
        common_allocation = allocation;
    std::array<std::uint8_t, kMinHookBlockSize> code{};
    if (!ExecutableRange(process, trampoline, code.size()) ||
        !ReadExact(process, trampoline, code.data(), code.size()))
        return Fail(failure, stage, "trampoline_read", index, trampoline,
                    0, 0, GetLastError());
    if (std::memcmp(code.data(), state.target_prefix.data(),
                    state.target_prefix.size()) != 0)
        return Fail(failure, stage, "trampoline_prefix", index, trampoline,
                    PrefixValue(code.data(), state.target_prefix.size()),
                    PrefixValue(state.target_prefix.data(),
                                state.target_prefix.size()));
    std::size_t jumpback = code.size();
    for (std::size_t offset = kTargetPrefixSize;
         offset + 14 <= code.size(); ++offset) {
        std::uint64_t target = 0;
        if (AbsoluteJump(code.data() + offset, code.size() - offset, target) &&
            target >= state.target + kTargetPrefixSize &&
            target <= state.target + 0x20) {
            jumpback = offset;
            break;
        }
    }
    if (jumpback == code.size())
        return Fail(failure, stage, "jumpback_present", index, trampoline);
    const std::uint64_t expected_relay = trampoline + jumpback + 14;
    std::uint64_t relay = expected_relay;
    std::array<std::uint8_t, kTargetPrefixSize> patch{};
    if (!RangeInModule(state.target, patch.size(), module) ||
        !ReadExact(process, state.target, patch.data(), patch.size()))
        return Fail(failure, stage, "target_patch_read", index,
                    state.target, 0, 0, GetLastError());
    if (target_enabled) {
        if (patch[0] != 0xE9)
            return Fail(failure, stage, "target_patch_opcode", index,
                        state.target, patch[0], 0xE9);
        std::int32_t displacement = 0;
        std::memcpy(&displacement, patch.data() + 1,
                    sizeof(displacement));
        const std::int64_t relay_signed =
            static_cast<std::int64_t>(state.target + patch.size()) +
            displacement;
        if (relay_signed <= 0)
            return Fail(failure, stage, "relay_positive", index,
                        state.target,
                        static_cast<std::uint64_t>(relay_signed), 1);
        relay = static_cast<std::uint64_t>(relay_signed);
    } else if (patch != state.target_prefix) {
        return Fail(failure, stage, "target_original_exact", index,
                    state.target,
                    PrefixValue(patch.data(), patch.size()),
                    PrefixValue(state.target_prefix.data(),
                                state.target_prefix.size()));
    }
    if (relay != expected_relay || relay < trampoline ||
        relay + 14 < relay || relay + 14 > trampoline + code.size())
        return Fail(failure, stage, "relay_layout", index, relay,
                    relay, expected_relay);
    std::uint64_t relay_target = 0;
    if (!AbsoluteJump(code.data() + (relay - trampoline),
                      code.size() - (relay - trampoline), relay_target) ||
        relay_target != state.detour)
        return Fail(failure, stage, "relay_target", index, relay,
                    relay_target, state.detour);
    std::array<std::uint8_t, kDetourPrefixSize> detour{};
    if (!ReadExact(process, state.detour, detour.data(), detour.size()) ||
        detour != state.detour_prefix)
        return Fail(failure, stage, "detour_prefix", index, state.detour,
                    PrefixValue(detour.data(), detour.size()),
                    PrefixValue(state.detour_prefix.data(),
                                state.detour_prefix.size()),
                    GetLastError());
    return true;
}

bool PrepareMinHook(HANDLE process, std::uint64_t image_base,
                    const MinHookSpec& spec, const ModuleInfo& module,
                    std::uint32_t target_rva,
                    MinState& state, std::uint64_t& common_allocation,
                    Failure& failure, std::size_t index) {
    state.installed = false;
    state.created_present = false;
    state.target = module.base + target_rva;
    state.detour = image_base + spec.detour_rva;
    state.saved_slot = image_base + spec.saved_slot_rva;
    if (!RangeInModule(state.target, kTargetPrefixSize, module) ||
        !ExecutableRange(process, state.target, kTargetPrefixSize) ||
        spec.detour_rva >= kActiveImageSize ||
        kDetourPrefixSize > kActiveImageSize - spec.detour_rva ||
        !ExecutableRange(process, state.detour, kDetourPrefixSize) ||
        !ReadExact(process, state.detour, state.detour_prefix.data(),
                   state.detour_prefix.size()))
        return Fail(failure, "minhook_pre", "range_prefix", index,
                    state.target, 0, module.base, GetLastError());
    std::uint64_t trampoline = 0;
    if (!ReadValue(process, state.saved_slot, trampoline))
        return Fail(failure, "minhook_pre", "saved_read", index,
                    state.saved_slot, 0, 0, GetLastError());
    std::array<std::uint8_t, kTargetPrefixSize> live{};
    if (!ReadExact(process, state.target, live.data(), live.size()))
        return Fail(failure, "minhook_pre", "target_read", index,
                    state.target, 0, 0, GetLastError());
    if (!trampoline && live[0] != 0xE9) {
        state.target_prefix = live;
        return true;
    }
    if (!trampoline)
        return Fail(failure, "minhook_pre", "saved_target_consistent", index,
                    state.saved_slot, trampoline, live[0]);
    if (!ReadExact(process, trampoline, state.target_prefix.data(),
                   state.target_prefix.size()))
        return Fail(failure, "minhook_pre", "trampoline_prefix_read", index,
                    trampoline, 0, 0, GetLastError());
    state.created_present = true;
    if (live[0] != 0xE9)
        return VerifyMinHook(process, module, state, common_allocation,
                             failure, index, "minhook_pre_created",
                             false);
    state.installed = true;
    return VerifyMinHook(process, module, state, common_allocation,
                         failure, index, "minhook_pre");
}

bool ApplyMinHook(HANDLE process, std::uint64_t image_base,
                  MinState& state, Failure& failure, std::size_t index) {
    if (state.installed)
        return true;
    std::uint64_t result = UINT64_MAX;
    if (!state.created_present) {
        const bool created_call =
            RemoteCall3(process, image_base + kCreateHookRva,
                        state.target, state.detour, state.saved_slot,
                        result, failure, "minhook_create_call", index);
        std::uint64_t trampoline = 0;
        const bool trampoline_read =
            ReadValue(process, state.saved_slot, trampoline);
        state.created = trampoline_read && trampoline != 0;
        state.created_present = state.created;
        if (!created_call)
            return false;
        if (static_cast<std::uint32_t>(result) != 0)
            return Fail(failure, "minhook_create", "status_zero", index,
                        state.target,
                        static_cast<std::uint32_t>(result), 0);
        if (!trampoline_read || !trampoline)
            return Fail(failure, "minhook_create", "saved_nonzero", index,
                        state.saved_slot, trampoline, 1, GetLastError());
    }
    result = UINT64_MAX;
    const bool enabled_call = RemoteCall3(
        process, image_base + kEnableHookRva, state.target, 1, 0,
        result, failure, "minhook_enable_call", index);
    std::array<std::uint8_t, kTargetPrefixSize> live{};
    state.enabled = ReadExact(process, state.target, live.data(), live.size()) &&
                    live[0] == 0xE9;
    if (!enabled_call)
        return false;
    if (static_cast<std::uint32_t>(result) != 0)
        return Fail(failure, "minhook_enable", "status_zero", index,
                    state.target, static_cast<std::uint32_t>(result), 0);
    if (!state.enabled)
        return Fail(failure, "minhook_enable", "target_patched", index,
                    state.target, live[0], 0xE9, GetLastError());
    return true;
}

bool Rollback(HANDLE process, std::uint64_t image_base,
               std::array<InterfaceRequest, 4>& interfaces,
               std::array<SteamInterfaceState, kSteamInterfaces.size()>&
                   steam_interfaces,
               std::array<DirectState, kDirectHooks.size()>& directs,
               ModelRenderGuardState& model_render_guard,
               PredictionGuardState& prediction_guard,
               ClientCommandListGuardState& client_command_list_guard,
               BacktrackPostMoveState& backtrack_post_move,
               ReplacementState& replacement,
               LuaBootstrapState& lua_bootstrap,
               ConfigBindingsState& config_bindings,
               std::array<CloneState, kCloneHooks.size()>& clones,
               void* generic_owner_page,
               std::array<MinState, kMinHooks.size()>& minhooks,
               std::array<DynamicMinHookState, 2>& dynamic_hooks,
               DynamicMinHookState& mouse_hook,
               EventState& event,
               std::array<ImagePatchState, kImagePatches.size()>&
                   image_patches) {
    bool ok = true;
    Failure ignored{};
    constexpr std::size_t lua_debug_hook_index = kMinHooks.size() - 2;
    constexpr std::size_t lua_close_hook_index = kMinHooks.size() - 1;
    if (!RollbackDynamicMinHooks(process, image_base, dynamic_hooks,
                                 mouse_hook))
        return false;
    if (!RollbackModelRenderGuard(process, model_render_guard))
        return false;
    if (!RemoteCallsRollbackSafe())
        return false;
    auto remove_min_hook = [&](std::size_t index) {
        auto& state = minhooks[index];
        std::uint64_t result = UINT64_MAX;
        if (state.enabled) {
            bool completed = false;
            const bool called = RemoteCall3(
                process, image_base + kEnableHookRva, state.target, 0, 0,
                result, ignored, "rollback_disable", index, &completed);
            if (!RemoteCallsRollbackSafe() || !completed ||
                static_cast<std::uint32_t>(result) != 0)
                return false;
            if (!called)
                ok = false;
            state.enabled = false;
        }
        if (state.created) {
            result = UINT64_MAX;
            bool completed = false;
            const bool called = RemoteCall3(
                process, image_base + kRemoveHookRva, state.target, 0, 0,
                result, ignored, "rollback_remove", index, &completed);
            if (!RemoteCallsRollbackSafe() || !completed ||
                static_cast<std::uint32_t>(result) != 0)
                return false;
            if (!called)
                ok = false;
            if (!WriteProtectedQword(process, state.saved_slot, 0))
                ok = false;
            std::array<std::uint8_t, kTargetPrefixSize> restored{};
            if (!ReadExact(process, state.target, restored.data(),
                           restored.size()) ||
                restored != state.target_prefix)
                ok = false;
            state.created = false;
        }
        return true;
    };
    if (!RollbackEventHook(process, image_base, event))
        ok = false;
    for (std::size_t reverse = minhooks.size(); reverse != 0; --reverse) {
        const std::size_t index = reverse - 1;
        if (index == lua_close_hook_index || index == lua_debug_hook_index)
            continue;
        if (!remove_min_hook(index))
            return false;
    }
    if (!RollbackCloneHooks(process, image_base, clones,
                            generic_owner_page))
        ok = false;
    if (!RollbackBacktrackPostMove(process, backtrack_post_move))
        ok = false;
    if (!RemoteCallsRollbackSafe())
        return false;
    if (!RollbackLuaBootstrap(process, image_base, lua_bootstrap))
        ok = false;
    if (!RemoteCallsRollbackSafe())
        return false;
    if (replacement.changed &&
        !WriteProtectedQword(process, replacement.slot,
                             replacement.original))
        ok = false;
    for (std::size_t reverse = directs.size(); reverse != 0; --reverse) {
        auto& state = directs[reverse - 1];
        if (state.entry_changed) {
            if (!WriteProtectedQword(process, state.entry_slot,
                                     state.original)) {
                ok = false;
                continue;
            }
            state.entry_changed = false;
        }
        if (state.saved_changed) {
            if (!WriteProtectedQword(
                    process,
                    image_base + kDirectHooks[reverse - 1].saved_slot_rva,
                    0)) {
                ok = false;
                continue;
            }
            state.saved_changed = false;
            state.saved_present = false;
        }
        state.changed = false;
    }
    if (!RollbackPredictionGuard(process, prediction_guard))
        ok = false;
    if (!RemoteCallsRollbackSafe())
        return false;
    if (!remove_min_hook(lua_debug_hook_index) ||
        !remove_min_hook(lua_close_hook_index))
        return false;
    if (!RollbackConfigBindings(process, image_base, config_bindings))
        return false;
    if (!RemoteCallsRollbackSafe())
        return false;
    if (!RollbackSteamInterfaces(process, image_base, steam_interfaces))
        ok = false;
    for (std::size_t reverse = interfaces.size(); reverse != 0; --reverse) {
        auto& request = interfaces[reverse - 1];
        if (request.changed &&
            !WriteProtectedQword(process,
                                 image_base + request.slot_rva,
                                 request.previous))
            ok = false;
    }
    if (!RollbackImagePatches(process, image_patches))
        ok = false;
    if (!RollbackClientCommandListGuard(process,
                                        client_command_list_guard))
        ok = false;
    return ok;
}

extern "C" std::uint32_t KirkwareDeferredEventWorker(
    KirkwareDeferredEventContext* context);
extern "C" const std::uint8_t KirkwareDeferredEventWorkerEnd[];

bool ScheduleRemoteDeferredEvent(DWORD pid, HANDLE process,
                                 std::uint64_t image_base,
                                 const LuaBootstrapState& lua_bootstrap,
                                 const EventState& event, Failure& failure,
                                 bool& rollback_safe) {
    rollback_safe = true;
    if (!lua_bootstrap.original ||
        lua_bootstrap.realm_vtable_count == 0 ||
        lua_bootstrap.realm_vtable_count >
            lua_bootstrap.realm_vtables.size())
        return Fail(failure, "event_deferred", "lua_topology", 0,
                    lua_bootstrap.original,
                    lua_bootstrap.realm_vtable_count, 1);
    if (event.installed)
        return true;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return Fail(failure, "event_deferred", "process_time", 0, pid,
                    0, 1, GetLastError());
    const std::uint64_t creation_time =
        static_cast<std::uint64_t>(created.dwLowDateTime) |
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32);
    const std::wstring mutex_name =
        L"Local\\kirkwareEventWorker-" + std::to_wstring(pid) + L"-" +
        std::to_wstring(creation_time);
    SetLastError(ERROR_SUCCESS);
    HANDLE raw_mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    const DWORD mutex_error = GetLastError();
    Handle mutex(raw_mutex);
    if (!mutex)
        return Fail(failure, "event_deferred", "mutex_create", 0, pid,
                    0, 1, mutex_error);
    const std::uint64_t detour = image_base + kEventHook.detour_rva;
    const std::uint64_t saved_slot =
        image_base + kEventHook.saved_slot_rva;
    if (mutex_error == ERROR_ALREADY_EXISTS) {
        for (unsigned attempt = 0; attempt != 400; ++attempt) {
            std::uint64_t object = 0;
            std::uint64_t vtable = 0;
            std::uint64_t live = 0;
            std::uint64_t saved = 0;
            EventVector vector{};
            std::vector<EventRecord> records;
            if (ReadValue(process,
                          image_base + kEventHook.object_slot_rva, object) &&
                object == event.object &&
                ReadValue(process, object, vtable) &&
                vtable == event.vtable &&
                ReadValue(process, event.entry_slot, live) &&
                ReadValue(process, saved_slot, saved) &&
                ReadEventVectorState(process, image_base, vector) &&
                ReadEventRecords(process, vector, records)) {
                std::size_t slot_count = 0;
                std::size_t exact_count = 0;
                std::size_t exact_index = records.size();
                CountEventRecords(records, event.object,
                                  kEventHook.vtable_index, event.original,
                                  slot_count, exact_count, exact_index);
                if (saved == event.original && live == event.original &&
                    slot_count == 0)
                    return true;
                if (saved == event.original && live == detour &&
                    slot_count == 1 && exact_count == 1 &&
                    exact_index + 1 == records.size())
                    return true;
                if ((saved && saved != event.original) ||
                    (live != event.original && live != detour) ||
                    (live == event.original && slot_count != 0))
                    return Fail(failure, "event_deferred",
                                "existing_worker_conflict", 0,
                                event.entry_slot, live, detour);
            }
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                return Fail(failure, "event_deferred",
                            "existing_worker_process_alive", 0, pid, 0, 1);
            Sleep(25);
        }
        return Fail(failure, "event_deferred",
                    "existing_worker_unconfirmed", 0, saved_slot, 0,
                    event.original, WAIT_TIMEOUT);
    }
    if (event.pending) {
        std::uint64_t object = 0;
        std::uint64_t vtable = 0;
        std::uint64_t live = 0;
        std::uint64_t saved = 0;
        EventVector vector{};
        std::vector<EventRecord> records;
        if (!ReadValue(process,
                       image_base + kEventHook.object_slot_rva, object) ||
            object != event.object || !ReadValue(process, object, vtable) ||
            vtable != event.vtable ||
            !ReadValue(process, event.entry_slot, live) ||
            !ReadValue(process, saved_slot, saved) ||
            !ReadEventVectorState(process, image_base, vector) ||
            !ReadEventRecords(process, vector, records))
            return Fail(failure, "event_deferred", "pending_revalidate", 0,
                        event.entry_slot, live, event.original,
                        GetLastError());
        std::size_t slot_count = 0;
        std::size_t exact_count = 0;
        std::size_t exact_index = records.size();
        CountEventRecords(records, event.object, kEventHook.vtable_index,
                          event.original, slot_count, exact_count,
                          exact_index);
        if (live == detour && saved == event.original && slot_count == 1 &&
            exact_count == 1 && exact_index + 1 == records.size())
            return true;
        if (live != event.original || saved != event.original || slot_count ||
            !SameEventVector(vector, event.vector_before) ||
            !SameEventRecords(records, event.records_before))
            return Fail(failure, "event_deferred", "pending_state_exact", 0,
                        event.entry_slot, live, event.original);
        if (!WriteProtectedQword(process, saved_slot, 0))
            return Fail(failure, "event_deferred", "stale_claim_clear", 0,
                        saved_slot, event.original, 0, GetLastError());
    }
    const ModuleInfo kernelbase = FindModule(pid, "kernelbase.dll");
    const std::uint64_t sleep_rva = ExportRva(kernelbase, "Sleep");
    const std::uint64_t read_rva =
        ExportRva(kernelbase, "ReadProcessMemory");
    const std::uint64_t write_rva =
        ExportRva(kernelbase, "WriteProcessMemory");
    const std::uint64_t protect_rva =
        ExportRva(kernelbase, "VirtualProtect");
    const std::uint64_t close_rva = ExportRva(kernelbase, "CloseHandle");
    const std::uint64_t sleep_function = kernelbase.base + sleep_rva;
    const std::uint64_t read_function = kernelbase.base + read_rva;
    const std::uint64_t write_function = kernelbase.base + write_rva;
    const std::uint64_t protect_function = kernelbase.base + protect_rva;
    const std::uint64_t close_function = kernelbase.base + close_rva;
    if (!kernelbase.base || !sleep_rva || !read_rva || !write_rva ||
        !protect_rva || !close_rva ||
        !RangeInModule(sleep_function, 1, kernelbase) ||
        !RangeInModule(read_function, 1, kernelbase) ||
        !RangeInModule(write_function, 1, kernelbase) ||
        !RangeInModule(protect_function, 1, kernelbase) ||
        !RangeInModule(close_function, 1, kernelbase) ||
        !ExecutableRange(process, sleep_function) ||
        !ExecutableRange(process, read_function) ||
        !ExecutableRange(process, write_function) ||
        !ExecutableRange(process, protect_function) ||
        !ExecutableRange(process, close_function))
        return Fail(failure, "event_deferred", "kernelbase_exports", 0,
                    kernelbase.base,
                    sleep_rva | read_rva | write_rva | protect_rva |
                        close_rva,
                    1,
                    GetLastError());

    const std::uint64_t worker_begin = reinterpret_cast<std::uint64_t>(
        KirkwareDeferredEventWorker);
    const std::uint64_t worker_end = reinterpret_cast<std::uint64_t>(
        KirkwareDeferredEventWorkerEnd);
    if (worker_end <= worker_begin || worker_end - worker_begin > 0x10000)
        return Fail(failure, "event_deferred", "worker_size", 0,
                    worker_begin, worker_end, 0x10000);
    const std::size_t worker_size =
        static_cast<std::size_t>(worker_end - worker_begin);

    const std::size_t records_bytes =
        event.records_before.size() * sizeof(EventRecord);
    if (records_bytes > 4096 * sizeof(EventRecord) ||
        records_bytes > SIZE_MAX - sizeof(EventRecord))
        return Fail(failure, "event_deferred", "records_size", 0,
                    records_bytes, event.records_before.size(), 4096);
    const std::size_t context_header =
        (sizeof(KirkwareDeferredEventContext) + 15) &
        ~static_cast<std::size_t>(15);
    const std::size_t current_capacity = records_bytes + sizeof(EventRecord);
    if (context_header > SIZE_MAX - records_bytes ||
        context_header + records_bytes > SIZE_MAX - current_capacity)
        return Fail(failure, "event_deferred", "context_size", 0,
                    context_header, records_bytes, current_capacity);
    const std::size_t context_size =
        context_header + records_bytes + current_capacity;

    void* remote_context = VirtualAllocEx(
        process, nullptr, context_size, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    void* remote_code = VirtualAllocEx(
        process, nullptr, worker_size, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!remote_context || !remote_code) {
        const DWORD error = GetLastError();
        if (remote_context)
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        if (remote_code)
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "remote_allocate", 0,
                    0, 0, context_size + worker_size, error);
    }

    KirkwareDeferredEventContext context{};
    context.sleep_function = sleep_function;
    context.read_process_memory = read_function;
    context.write_process_memory = write_function;
    context.virtual_protect = protect_function;
    context.close_handle = close_function;
    context.installer_function = image_base + 0x33D620;
    context.image_base = image_base;
    context.event_object = event.object;
    context.event_vtable = event.vtable;
    context.event_entry_slot = event.entry_slot;
    context.event_original = event.original;
    context.event_detour = image_base + kEventHook.detour_rva;
    context.event_saved_slot = image_base + kEventHook.saved_slot_rva;
    context.lua_detour = image_base + 0x31DE30;
    context.lua_vtable_a = lua_bootstrap.realm_vtables[0];
    context.lua_vtable_b = lua_bootstrap.realm_vtable_count > 1
        ? lua_bootstrap.realm_vtables[1]
        : lua_bootstrap.realm_vtables[0];
    context.lua_original = lua_bootstrap.original;
    context.lua_original_prefix_a =
        PrefixValue(kLuaRunStringOriginalPrefix, 8);
    context.lua_original_prefix_b =
        PrefixValue(kLuaRunStringOriginalPrefix + 8, 8);
    context.lua_detour_prefix_a = PrefixValue(kDetour31de30, 8);
    context.lua_detour_prefix_b = PrefixValue(kDetour31de30 + 8, 8);
    context.event_vector_slots = image_base + kEventRecordsBeginRva;
    context.expected_vector_begin = event.vector_before.begin;
    context.expected_vector_end = event.vector_before.end;
    context.expected_vector_capacity = event.vector_before.capacity;
    context.expected_records_address =
        reinterpret_cast<std::uint64_t>(remote_context) + context_header;
    context.current_records_address =
        context.expected_records_address + records_bytes;
    context.expected_records_bytes = records_bytes;
    context.current_records_capacity = current_capacity;

    if (!WriteExact(process, reinterpret_cast<std::uint64_t>(remote_context),
                    &context, sizeof(context)) ||
        (records_bytes &&
         !WriteExact(process, context.expected_records_address,
                     event.records_before.data(), records_bytes)) ||
        !WriteExact(process, reinterpret_cast<std::uint64_t>(remote_code),
                    reinterpret_cast<const void*>(worker_begin), worker_size)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "remote_write", 0,
                    0, 0, worker_size, error);
    }
    DWORD previous = 0;
    if (!VirtualProtectEx(process, remote_code, worker_size, PAGE_EXECUTE_READ,
                          &previous) ||
        !FlushInstructionCache(process, remote_code, worker_size)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "remote_code_prepare", 0,
                    reinterpret_cast<std::uint64_t>(remote_code), 0,
                    worker_size, error);
    }
    Handle thread(CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_code), remote_context,
        0, nullptr));
    if (!thread) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "remote_thread_create", 0,
                    reinterpret_cast<std::uint64_t>(remote_code), 0, 1,
                    error);
    }

    const std::uint64_t worker_status_slot =
        reinterpret_cast<std::uint64_t>(remote_context) +
        offsetof(KirkwareDeferredEventContext, status);
    const std::uint64_t worker_acknowledged_slot =
        reinterpret_cast<std::uint64_t>(remote_context) +
        offsetof(KirkwareDeferredEventContext, acknowledged);
    DWORD thread_state = WaitForSingleObject(thread.Get(), 0);
    if (thread_state == WAIT_OBJECT_0) {
        std::uint32_t worker_status = 0;
        ReadValue(process, worker_status_slot, worker_status);
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "worker_early_exit", 0,
                    worker_status_slot, worker_status, 0, ERROR_GEN_FAILURE);
    }
    if (thread_state == WAIT_FAILED) {
        rollback_safe = false;
        return Fail(failure, "event_deferred", "worker_state", 0,
                    reinterpret_cast<std::uint64_t>(remote_code), 0,
                    WAIT_TIMEOUT, GetLastError());
    }

    HANDLE remote_mutex = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), mutex.Get(), process,
                         &remote_mutex, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        const DWORD error = GetLastError();
        if (WaitForSingleObject(thread.Get(), 15000) == WAIT_OBJECT_0) {
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        }
        return Fail(failure, "event_deferred", "mutex_duplicate", 0,
                    pid, 0, 1, error);
    }

    const std::uint64_t remote_mutex_value =
        reinterpret_cast<std::uint64_t>(remote_mutex);
    const std::uint64_t remote_mutex_slot =
        reinterpret_cast<std::uint64_t>(remote_context) +
        offsetof(KirkwareDeferredEventContext, mutex_handle);
    auto reclaim_remote_mutex = [&]() {
        if (!remote_mutex)
            return true;
        HANDLE reclaimed = nullptr;
        const bool reclaimed_ok =
            DuplicateHandle(process, remote_mutex, GetCurrentProcess(),
                            &reclaimed, 0, FALSE,
                            DUPLICATE_SAME_ACCESS |
                                DUPLICATE_CLOSE_SOURCE) != FALSE;
        remote_mutex = nullptr;
        if (reclaimed)
            CloseHandle(reclaimed);
        return reclaimed_ok;
    };
    if (!WriteExact(process, remote_mutex_slot, &remote_mutex_value,
                    sizeof(remote_mutex_value))) {
        const DWORD error = GetLastError();
        std::uint64_t observed_mutex = UINT64_MAX;
        const bool mutex_slot_read =
            ReadValue(process, remote_mutex_slot, observed_mutex);
        if (mutex_slot_read && observed_mutex == 0)
            reclaim_remote_mutex();
        if (WaitForSingleObject(thread.Get(), 15000) == WAIT_OBJECT_0) {
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        }
        return Fail(failure, "event_deferred", "mutex_publish", 0,
                    remote_mutex_slot, observed_mutex, remote_mutex_value,
                    error ? error : ERROR_WRITE_FAULT);
    }

    thread_state = WaitForSingleObject(thread.Get(), 0);
    if (thread_state == WAIT_OBJECT_0) {
        std::uint32_t worker_status = 0;
        std::uint64_t observed_mutex = 0;
        ReadValue(process, worker_status_slot, worker_status);
        if (ReadValue(process, remote_mutex_slot, observed_mutex) &&
            observed_mutex == remote_mutex_value)
            reclaim_remote_mutex();
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
        return Fail(failure, "event_deferred", "worker_prearm_exit", 0,
                    worker_status_slot, worker_status, 0, ERROR_GEN_FAILURE);
    }
    if (thread_state == WAIT_FAILED) {
        rollback_safe = false;
        return Fail(failure, "event_deferred", "worker_prearm_state", 0,
                    reinterpret_cast<std::uint64_t>(remote_code), 0,
                    WAIT_TIMEOUT, GetLastError());
    }

    constexpr std::uint32_t armed = 1;
    const std::uint64_t armed_slot =
        reinterpret_cast<std::uint64_t>(remote_context) +
        offsetof(KirkwareDeferredEventContext, armed);
    const bool arm_written =
        WriteExact(process, armed_slot, &armed, sizeof(armed));
    const DWORD arm_write_error = arm_written ? ERROR_SUCCESS : GetLastError();
    bool acknowledged = false;
    std::uint32_t observed_acknowledged = 0;
    for (unsigned attempt = 0; attempt != 480; ++attempt) {
        if (ReadValue(process, worker_acknowledged_slot,
                      observed_acknowledged) &&
            observed_acknowledged == 1)
            acknowledged = true;
        thread_state = WaitForSingleObject(thread.Get(), 0);
        if (acknowledged && thread_state == WAIT_TIMEOUT)
            goto worker_scheduled;
        if (thread_state == WAIT_OBJECT_0) {
            std::uint32_t worker_status = 0;
            ReadValue(process, worker_status_slot, worker_status);
            if (acknowledged && worker_status == 1)
                goto worker_scheduled;
            rollback_safe = worker_status == 3 || worker_status == 5 ||
                            worker_status == 6 || worker_status == 8 ||
                            worker_status == 9;
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
            VirtualFreeEx(process, remote_code, 0, MEM_RELEASE);
            return Fail(failure, "event_deferred", "worker_ack_exit", 0,
                        worker_status_slot, worker_status, 1,
                        ERROR_GEN_FAILURE);
        }
        if (thread_state == WAIT_FAILED) {
            rollback_safe = false;
            return Fail(failure, "event_deferred", "worker_ack_state", 0,
                        reinterpret_cast<std::uint64_t>(remote_code), 0,
                        WAIT_TIMEOUT, GetLastError());
        }
        Sleep(25);
    }
    rollback_safe = false;
    return Fail(failure, "event_deferred", "worker_ack_timeout", 0,
                worker_acknowledged_slot, observed_acknowledged, 1,
                arm_write_error ? arm_write_error : WAIT_TIMEOUT);
worker_scheduled:
    std::printf(
        "kirkware event worker context=0x%016llX code=0x%016llX "
        "code_bytes=%llu records=%llu\n",
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(remote_context)),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(remote_code)),
        static_cast<unsigned long long>(worker_size),
        static_cast<unsigned long long>(event.records_before.size()));
    return true;
}

bool Install(DWORD pid, std::uint64_t image_base,
             const KirkwareGameHookInterfaces* supplied,
             Failure& failure) {
    const ModuleInfo client = FindModule(pid, "client.dll");
    const ModuleInfo engine = FindModule(pid, "engine.dll");
    const ModuleInfo lua_shared = FindModule(pid, "lua_shared.dll");
    const ModuleInfo vgui2 = FindModule(pid, "vgui2.dll");
    const ModuleInfo studiorender = FindModule(pid, "studiorender.dll");
    const ModuleInfo steam_api = FindModule(pid, "steam_api64.dll");
    if (!client.base || !engine.base || !lua_shared.base || !vgui2.base ||
        !studiorender.base || !steam_api.base)
        return Fail(failure, "modules", "all_present", 0, pid,
                    static_cast<std::uint64_t>(!!client.base) |
                        (static_cast<std::uint64_t>(!!engine.base) << 1) |
                        (static_cast<std::uint64_t>(!!lua_shared.base) << 2) |
                        (static_cast<std::uint64_t>(!!vgui2.base) << 3) |
                        (static_cast<std::uint64_t>(!!studiorender.base) << 4) |
                        (static_cast<std::uint64_t>(!!steam_api.base) << 5),
                    0x3F);
    Handle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            PROCESS_DUP_HANDLE | SYNCHRONIZE,
        FALSE, pid));
    if (!process)
        return Fail(failure, "process", "open", 0, pid, 0, 1,
                    GetLastError());
    bool remote_calls_rollback_safe = true;
    RemoteCallSafetyScope remote_call_scope(remote_calls_rollback_safe);
    if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
        return Fail(failure, "process", "alive", 0, pid, 0, 1);
    if (!ValidateImage(process.Get(), image_base, failure))
        return false;
    std::array<std::uint32_t, kMinHooks.size()> resolved_min_targets{};
    if (!ResolveMinHookTargets(process.Get(), image_base, client, engine,
                               lua_shared, resolved_min_targets, failure))
        return false;

    std::array<InterfaceRequest, 4> interfaces{{
        {&client, "VClient017", 0x7EA810,
         supplied ? supplied->client : 0},
        {&engine, "VEngineModel016", 0x7EA830,
         supplied ? supplied->model : 0},
        {&lua_shared, "LUASHARED003", 0x7EA8C0,
         supplied ? supplied->lua_shared : 0},
        {&vgui2, "VGUI_Panel009", 0x7EA858,
         supplied ? supplied->panel : 0},
    }};
    for (std::size_t index = 0; index != interfaces.size(); ++index) {
        auto& request = interfaces[index];
        if (request.result
                ? !ValidateInterfaceResult(process.Get(), request, index,
                                           failure)
                : !ResolveInterface(process.Get(), request, index, failure))
            return false;
        const std::uint64_t slot = image_base + request.slot_rva;
        if (!ReadValue(process.Get(), slot, request.previous))
            return Fail(failure, "interface_slot", "read", index, slot,
                        0, 0, GetLastError());
        if (request.previous != 0 && request.previous != request.result)
            return Fail(failure, "interface_slot", "zero_or_same", index,
                        slot, request.previous, request.result);
    }

    std::array<SteamInterfaceState, kSteamInterfaces.size()>
        steam_interfaces{};
    for (std::size_t index = 0; index != kSteamInterfaces.size(); ++index) {
        if (!PrepareSteamInterface(process.Get(), image_base, steam_api,
                                   kSteamInterfaces[index],
                                   steam_interfaces[index], index, failure))
            return false;
    }

    std::array<DirectState, kDirectHooks.size()> directs{};
    ModelRenderGuardState model_render_guard{};
    PredictionGuardState prediction_guard{};
    ClientCommandListGuardState client_command_list_guard{};
    BacktrackPostMoveState backtrack_post_move{};
    ReplacementState replacement{};
    LuaBootstrapState lua_bootstrap{};
    ConfigBindingsState config_bindings{};
    std::array<CloneState, kCloneHooks.size()> clones{};
    void* generic_owner_page = nullptr;
    std::array<MinState, kMinHooks.size()> minhooks{};
    std::array<DynamicMinHookState, 2> dynamic_hooks{};
    DynamicMinHookState mouse_hook{};
    EventState event{};
    std::array<ImagePatchState, kImagePatches.size()> image_patches{};
    auto abort = [&]() {
        if (!remote_calls_rollback_safe) {
            std::fprintf(stderr,
                         "kirkware hook rollback skipped because a "
                         "remote call remains active\n");
            return false;
        }
        if (!Rollback(process.Get(), image_base, interfaces,
                       steam_interfaces, directs,
                       model_render_guard, prediction_guard,
                       client_command_list_guard,
                       backtrack_post_move, replacement, lua_bootstrap,
                       config_bindings, clones,
                       generic_owner_page, minhooks, dynamic_hooks,
                       mouse_hook, event,
                      image_patches))
            std::fprintf(stderr,
                         "kirkware hook rollback incomplete\n");
        return false;
    };

    if (!VerifyBytes(process.Get(), image_base + 0x31E46D,
                     kLuaCloseResetExact, sizeof(kLuaCloseResetExact),
                     failure, "lua_lifecycle", "close_reset_exact", 0))
        return abort();
    if (!PrepareClientCommandListGuard(process.Get(), client,
                                       client_command_list_guard, failure))
        return abort();
    if (!PreparePredictionGuard(process.Get(), image_base,
                                prediction_guard, failure))
        return abort();
    if (!PrepareImagePatches(process.Get(), image_base, image_patches,
                             failure))
        return abort();
    for (std::size_t index = 0; index != kImagePatches.size(); ++index) {
        if (!ApplyImagePatch(process.Get(), kImagePatches[index],
                             image_patches[index], failure, index))
            return abort();
    }
    if (!PrepareBacktrackPostMove(process.Get(), image_base,
                                  backtrack_post_move, failure) ||
        !ApplyBacktrackPostMove(process.Get(), image_base,
                                backtrack_post_move, failure))
        return abort();

    for (std::size_t index = 0; index != interfaces.size(); ++index) {
        auto& request = interfaces[index];
        if (request.previous == request.result)
            continue;
        if (!WriteProtectedQword(process.Get(), image_base + request.slot_rva,
                                 request.result)) {
            Fail(failure, "interface_slot", "write", index,
                 image_base + request.slot_rva, 0, request.result,
                 GetLastError());
            return abort();
        }
        request.changed = true;
    }
    for (std::size_t index = 0; index != kSteamInterfaces.size(); ++index) {
        if (!PublishSteamInterface(process.Get(), image_base,
                                   kSteamInterfaces[index],
                                   steam_interfaces[index], index, failure))
            return abort();
    }

    std::uint64_t engine_object = 0;
    if (!ReadValue(process.Get(), image_base + 0x7EA8A8, engine_object) ||
        !engine_object) {
        Fail(failure, "engine_interface", "slot_nonzero", 0,
             image_base + 0x7EA8A8, engine_object, 1, GetLastError());
        return abort();
    }

    for (std::size_t index = 0; index != kDirectHooks.size(); ++index) {
        const auto& spec = kDirectHooks[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        if (!PrepareDirect(process.Get(), image_base, spec, module,
                           directs[index], failure, index))
            return abort();
    }
    if (!PrepareReplacement(process.Get(), image_base, client, replacement,
                            failure))
        return abort();
    std::size_t missing_generic_count = 0;
    if (!PrepareCloneHooks(process.Get(), image_base, client, engine,
                           lua_shared, vgui2, clones,
                           missing_generic_count, failure) ||
        !PrepareEventHook(process.Get(), image_base, engine, event,
                          failure))
        return abort();
    std::uint64_t common_pre = 0;
    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        const auto& spec = kMinHooks[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        if (!PrepareMinHook(process.Get(), image_base, spec, module,
                            resolved_min_targets[index],
                            minhooks[index], common_pre, failure, index))
            return abort();
    }

    if (missing_generic_count) {
        generic_owner_page = VirtualAllocEx(
            process.Get(), nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!generic_owner_page) {
            Fail(failure, "clone_owner", "page_allocate", 0, 0, 0,
                 missing_generic_count, GetLastError());
            return abort();
        }
        std::size_t cell = 0;
        for (std::size_t index = 0; index != kCloneHooks.size(); ++index) {
            if (!clones[index].installed &&
                kCloneHooks[index].form == CloneInstallForm::Generic) {
                clones[index].owner =
                    reinterpret_cast<std::uint64_t>(generic_owner_page) +
                    cell * 0x10;
                ++cell;
            }
        }
        if (cell != missing_generic_count) {
            Fail(failure, "clone_owner", "cell_count", 0,
                 reinterpret_cast<std::uint64_t>(generic_owner_page), cell,
                 missing_generic_count);
            return abort();
        }
    }

    if (!ApplyPredictionGuard(process.Get(), image_base,
                              prediction_guard, failure))
        return abort();

    std::uint64_t initialize_result = UINT64_MAX;
    if (!RemoteCall3(process.Get(), image_base + kInitializeHookRva, 0, 0, 0,
                     initialize_result, failure,
                     "minhook_initialize_call", 0))
        return abort();
    const std::uint32_t initialize_status =
        static_cast<std::uint32_t>(initialize_result);
    if (initialize_status != 0 && initialize_status != 1) {
        Fail(failure, "minhook_initialize", "status_ok_or_initialized", 0,
             image_base + kInitializeHookRva, initialize_status, 0);
        return abort();
    }
    constexpr std::size_t lua_close_hook_index = kMinHooks.size() - 1;
    constexpr std::size_t lua_debug_hook_index = kMinHooks.size() - 2;
    if (!ApplyMinHook(process.Get(), image_base,
                      minhooks[lua_close_hook_index], failure,
                      lua_close_hook_index))
        return abort();
    if (!ApplyMinHook(process.Get(), image_base,
                      minhooks[lua_debug_hook_index], failure,
                      lua_debug_hook_index))
        return abort();
    if (!EnsureDynamicMinHookBindings(process.Get(), image_base,
                                      dynamic_hooks, failure))
        return abort();

    for (std::size_t index = 0; index != kDirectHooks.size(); ++index) {
        if (!ApplyDirect(process.Get(), image_base, kDirectHooks[index],
                         directs[index], failure, index))
            return abort();
    }
    if (!InstallModelRenderGuard(process.Get(), studiorender,
                                 model_render_guard, failure))
        return abort();
    if (!ApplyReplacement(process.Get(), image_base, replacement, failure))
        return abort();

    if (!EnsureConfigBindings(process.Get(), image_base, config_bindings,
                              failure))
        return abort();
    if (!EnsureConfigRegistryPrewarm(process.Get(), image_base, failure))
        return abort();
    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        const auto& spec = kMinHooks[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        if (!PrepareMinHook(process.Get(), image_base, spec, module,
                            resolved_min_targets[index],
                            minhooks[index], common_pre, failure, index))
            return abort();
    }

    if (!EnsureLuaLinkerRegistrars(pid, process.Get(), image_base, failure,
                                   remote_calls_rollback_safe))
        return abort();

    if (!PrepareLuaBootstrap(process.Get(), image_base, lua_shared,
                              interfaces[2].result, lua_bootstrap,
                              failure) ||
        !EnsureLuaRegistration(process.Get(), image_base, lua_bootstrap,
                               failure) ||
        !InstallLuaLinkerReplay(process.Get(), image_base, lua_bootstrap,
                                failure) ||
        !VerifyLuaBootstrap(process.Get(), image_base, lua_bootstrap,
                            failure))
        return abort();

    for (std::size_t index = 0; index != kCloneHooks.size(); ++index) {
        if (index == kSkippedCreateMoveCloneIndex)
            continue;
        if (!ApplyCloneHook(process.Get(), image_base, kCloneHooks[index],
                            clones[index], failure, index) ||
            !VerifyCloneGroupThrough(process.Get(), image_base, clones,
                                     index, failure))
            return abort();
    }

    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        if (index == lua_close_hook_index || index == lua_debug_hook_index)
            continue;
        if (index == kClMoveHookIndex)
            continue;
        if (!ApplyMinHook(process.Get(), image_base, minhooks[index], failure,
                          index))
            return abort();
    }
    if (!ApplyClientCommandListGuard(process.Get(),
                                     client_command_list_guard, failure))
        return abort();
    if (!ApplyMinHook(process.Get(), image_base,
                      minhooks[kClMoveHookIndex], failure,
                      kClMoveHookIndex))
        return abort();

    if (!EnsureMouseInputBinding(process.Get(), image_base, mouse_hook,
                                 failure))
        return abort();

    for (std::size_t index = 0; index != kDirectHooks.size(); ++index) {
        const auto& spec = kDirectHooks[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        if (!VerifyDirect(process.Get(), image_base, spec, module,
                          directs[index], failure, index))
            return abort();
    }
    if (!VerifyModelRenderGuard(process.Get(), model_render_guard, failure))
        return abort();
    if (!VerifyPredictionGuard(process.Get(), prediction_guard, failure))
        return abort();
    if (!VerifyClientCommandListGuard(process.Get(),
                                      client_command_list_guard, failure))
        return abort();
    if (!VerifyBacktrackPostMove(process.Get(), backtrack_post_move,
                                 failure))
        return abort();
    if (!VerifyReplacement(process.Get(), image_base, replacement, failure))
        return abort();
    if (!VerifyLuaBootstrap(process.Get(), image_base, lua_bootstrap,
                            failure) ||
        !VerifyCloneHooks(process.Get(), image_base, client, engine,
                          lua_shared, vgui2, clones, failure))
        return abort();
    std::uint8_t create_move_guard = 0;
    if (!ReadValue(process.Get(), image_base + 0x7EA565,
                   create_move_guard) ||
        create_move_guard != 1) {
        Fail(failure, "lua_lifecycle", "initial_guard", 0,
             image_base + 0x7EA565, create_move_guard, 1,
             GetLastError());
        return abort();
    }
    std::uint64_t common_post = 0;
    for (std::size_t index = 0; index != kMinHooks.size(); ++index) {
        const auto& spec = kMinHooks[index];
        const auto& module =
            ModuleFor(spec.module, client, engine, lua_shared, vgui2);
        if (!VerifyMinHook(process.Get(), module, minhooks[index],
                           common_post, failure, index, "minhook_post"))
            return abort();
    }
    if (!common_post) {
        Fail(failure, "minhook_post", "common_allocation_nonzero", 0,
             0, 0, 1);
        return abort();
    }
    for (std::size_t index = 0; index != interfaces.size(); ++index) {
        std::uint64_t observed = 0;
        const auto& request = interfaces[index];
        if (!ReadValue(process.Get(), image_base + request.slot_rva,
                       observed) ||
            observed != request.result) {
            Fail(failure, "interface_post", "slot_exact", index,
                 image_base + request.slot_rva, observed,
                 request.result, GetLastError());
            return abort();
        }
    }
    for (std::size_t index = 0; index != kSteamInterfaces.size(); ++index) {
        auto& state = steam_interfaces[index];
        std::uint64_t observed = 0;
        if (!ReadValue(process.Get(),
                       image_base + kSteamInterfaces[index].slot_rva,
                       observed) ||
            observed != state.published_object) {
            Fail(failure, "steam_final", "slot_exact", index,
                 image_base + kSteamInterfaces[index].slot_rva,
                 observed, state.published_object, GetLastError());
            return abort();
        }
        if (!ValidateSteamInterfaceObject(
                process.Get(), kSteamInterfaces[index], state, index,
                failure, "steam_final"))
            return abort();
    }
    if (event.pending || lua_bootstrap.deferred) {
        bool event_rollback_safe = true;
        if (!ScheduleRemoteDeferredEvent(pid, process.Get(), image_base,
                                         lua_bootstrap, event, failure,
                                         event_rollback_safe))
            return event_rollback_safe ? abort() : false;
    } else if (!ApplyEventHook(process.Get(), image_base, engine, event,
                               failure)) {
        return abort();
    }
    std::printf(
        "kirkware hooks installed pid=%lu image=0x%016llX "
        "interfaces=4 direct=6 lua=1 lua_registry=%u clones=8 event=%s replacement=1 "
        "minhook=%zu allocation=0x%016llX\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(image_base),
        lua_bootstrap.registration_seeded ? 1u : 0u,
        event.pending ? "target-pending" :
            (lua_bootstrap.deferred ? "target-deferred" : "1"),
        kMinHooks.size(),
        static_cast<unsigned long long>(common_post));
    return true;
}

}  

bool InstallKirkwareGameHooks(
    DWORD pid, std::uint64_t image_base,
    const KirkwareGameHookInterfaces* supplied) {
    Failure failure{};
    if (Install(pid, image_base, supplied, failure))
        return true;
    PrintFailure(failure);
    return false;
}

bool InstallKirkwareGameHooks(DWORD pid, std::uint64_t image_base) {
    return InstallKirkwareGameHooks(pid, image_base, nullptr);
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr,
                     "usage: install_kirkware_game_hooks.exe pid [image_base]\n");
        return 2;
    }
    char* end = nullptr;
    const unsigned long parsed_pid = std::strtoul(argv[1], &end, 0);
    if (!end || *end != '\0' || !parsed_pid || parsed_pid > MAXDWORD) {
        std::fprintf(stderr, "invalid pid\n");
        return 2;
    }
    std::uint64_t image_base = kDefaultImageBase;
    if (argc == 3) {
        end = nullptr;
        image_base = _strtoui64(argv[2], &end, 0);
        if (!end || *end != '\0' || !image_base) {
            std::fprintf(stderr, "invalid image base\n");
            return 2;
        }
    }
    return InstallKirkwareGameHooks(static_cast<DWORD>(parsed_pid),
                                         image_base)
               ? 0
               : 1;
}
