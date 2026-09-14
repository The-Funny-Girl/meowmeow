#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint64_t kImageBase = 0x000001E5DCC00000ull;
constexpr std::uint64_t kResolverRva = 0x56B30;
constexpr std::uint64_t kLuaTableRva = 0x7EA100;
constexpr std::uint64_t kCurrentLuaModuleRva = 0x7EA198;
constexpr std::size_t kFirstOwnedLuaSlot = 1;
constexpr std::size_t kOwnedLuaSlotCount = 13;

struct ModuleInfo {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::wstring path;
};

struct RemoteCallContext {
    std::uint64_t target = 0;
    std::uint64_t rcx = 0;
    std::uint64_t rdx = 0;
    std::uint64_t r8 = 0;
    std::uint64_t result = ~0ull;
};

ModuleInfo FindModule(DWORD pid, const wchar_t* requested) {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    ModuleInfo result{};
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, requested) == 0) {
                result.base =
                    reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
                result.size = entry.modBaseSize;
                result.path = entry.szExePath;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool ReadExact(HANDLE process, std::uint64_t address, void* data,
               std::size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             data, size, &read) != FALSE &&
           read == size;
}

bool WriteExact(HANDLE process, void* address, const void* data,
                std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, address, data, size, &written) != FALSE &&
           written == size;
}

bool BuildExpectedTable(const ModuleInfo& lua,
                        std::array<std::uint64_t, 16>& expected) {
    constexpr std::array<const char*, 13> names{{
        "luaL_checknumber", "lua_rawget", "lua_isstring", "lua_getfield",
        "lua_tonumber", "lua_isnumber", "lua_remove", "lua_gettop",
        "lua_settop", "lua_rawgeti", "lua_setfield", "lua_toboolean",
        "lua_pushvalue"}};
    HMODULE local = LoadLibraryExW(lua.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return false;
    bool valid = true;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto address = GetProcAddress(local, names[index]);
        if (!address) {
            valid = false;
            break;
        }
        const auto rva = reinterpret_cast<std::uint64_t>(address) -
                         reinterpret_cast<std::uint64_t>(local);
        if (rva >= lua.size) {
            valid = false;
            break;
        }
        expected[index + 1] = lua.base + rva;
    }
    FreeLibrary(local);
    return valid;
}

bool OwnedSlotsMatch(const std::array<std::uint64_t, 16>& actual,
                     const std::array<std::uint64_t, 16>& expected) {
    return std::equal(actual.begin() + kFirstOwnedLuaSlot,
                      actual.begin() + kFirstOwnedLuaSlot +
                          kOwnedLuaSlotCount,
                      expected.begin() + kFirstOwnedLuaSlot);
}

bool OwnedSlotsZero(const std::array<std::uint64_t, 16>& actual) {
    return std::all_of(actual.begin() + kFirstOwnedLuaSlot,
                       actual.begin() + kFirstOwnedLuaSlot +
                           kOwnedLuaSlotCount,
                       [](std::uint64_t value) { return value == 0; });
}

bool OwnedSlotsCompatible(const std::array<std::uint64_t, 16>& actual,
                          const std::array<std::uint64_t, 16>& expected) {
    for (std::size_t index = kFirstOwnedLuaSlot;
         index != kFirstOwnedLuaSlot + kOwnedLuaSlotCount; ++index) {
        if (actual[index] != 0 && actual[index] != expected[index])
            return false;
    }
    return true;
}

bool InvokeResolver(HANDLE process, std::uint64_t lua_base,
                    std::uint64_t& result, DWORD& thread_exit) {
    RemoteCallContext context{};
    context.target = kImageBase + kResolverRva;
    context.rcx = lua_base;
    constexpr std::array<std::uint8_t, 35> stub{{
        0x53,
        0x48, 0x83, 0xEC, 0x20,
        0x48, 0x89, 0xCB,
        0x48, 0x8B, 0x03,
        0x48, 0x8B, 0x4B, 0x08,
        0x48, 0x8B, 0x53, 0x10,
        0x4C, 0x8B, 0x43, 0x18,
        0xFF, 0xD0,
        0x48, 0x89, 0x43, 0x20,
        0x48, 0x83, 0xC4, 0x20,
        0x5B,
        0xC3}};
    void* remote_context = VirtualAllocEx(
        process, nullptr, sizeof(context), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    void* remote_stub = VirtualAllocEx(
        process, nullptr, stub.size(), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (!remote_context || !remote_stub ||
        !WriteExact(process, remote_context, &context, sizeof(context)) ||
        !WriteExact(process, remote_stub, stub.data(), stub.size())) {
        if (remote_stub)
            VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        if (remote_context)
            VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtectEx(process, remote_stub, stub.size(), PAGE_EXECUTE_READ,
                          &old_protect)) {
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(process, remote_stub, stub.size());
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub), remote_context,
        0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, 60000);
    const bool complete =
        wait == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &thread_exit) != FALSE &&
        ReadExact(process, reinterpret_cast<std::uint64_t>(remote_context),
                  &context, sizeof(context));
    if (complete) {
        result = context.result;
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_context, 0, MEM_RELEASE);
    }
    CloseHandle(thread);
    return complete;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: resolve_kirkware_lua_exports.exe <pid>\n");
        return 2;
    }
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    if (!pid)
        return 2;
    const auto lua = FindModule(pid, L"lua_shared.dll");
    if (!lua.base || !lua.size || lua.path.empty()) {
        std::fprintf(stderr, "lua_shared.dll is unavailable\n");
        return 1;
    }
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
        FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "resolver OpenProcess failed: %lu\n",
                     GetLastError());
        return 1;
    }
    std::array<std::uint64_t, 16> before{};
    std::uint64_t current_module_before = 0;
    if (!ReadExact(process, kImageBase + kLuaTableRva, before.data(),
                   sizeof(before)) ||
        !ReadExact(process, kImageBase + kCurrentLuaModuleRva,
                   &current_module_before, sizeof(current_module_before))) {
        std::fprintf(stderr, "Lua resolver pre-state read failed\n");
        CloseHandle(process);
        return 1;
    }
    std::array<std::uint64_t, 16> expected{};
    if (!BuildExpectedTable(lua, expected)) {
        std::fprintf(stderr, "current Lua export resolution failed\n");
        CloseHandle(process);
        return 1;
    }
    const bool zero_prestate = OwnedSlotsZero(before);
    const bool current_prestate = OwnedSlotsMatch(before, expected);
    if (!OwnedSlotsCompatible(before, expected)) {
        std::fprintf(stderr, "Lua export table has an unexpected pre-state\n");
        CloseHandle(process);
        return 1;
    }
    if (current_module_before != 0 && current_module_before != lua.base) {
        std::fprintf(stderr,
                     "current Lua module has an unexpected pre-state: "
                     "observed=0x%llX expected=0x%llX\n",
                     static_cast<unsigned long long>(current_module_before),
                     static_cast<unsigned long long>(lua.base));
        CloseHandle(process);
        return 1;
    }
    if (current_module_before == 0 &&
        !WriteExact(process,
                    reinterpret_cast<void*>(kImageBase +
                                            kCurrentLuaModuleRva),
                    &lua.base, sizeof(lua.base))) {
        std::fprintf(stderr, "current Lua module publication failed\n");
        CloseHandle(process);
        return 1;
    }
    std::uint64_t result = ~0ull;
    DWORD thread_exit = STILL_ACTIVE;
    if (!InvokeResolver(process, lua.base, result, thread_exit) ||
        result != 0 || thread_exit != 0) {
        std::fprintf(
            stderr,
            "aggregate Lua resolver failed: exit=0x%08lX result=0x%llX\n",
            thread_exit, static_cast<unsigned long long>(result));
        CloseHandle(process);
        return 1;
    }
    std::array<std::uint64_t, 16> after{};
    std::uint64_t current_module_after = 0;
    if (!ReadExact(process, kImageBase + kLuaTableRva, after.data(),
                    sizeof(after)) ||
        !ReadExact(process, kImageBase + kCurrentLuaModuleRva,
                   &current_module_after, sizeof(current_module_after)) ||
        !OwnedSlotsMatch(after, expected) || current_module_after != lua.base) {
        std::fprintf(stderr, "aggregate Lua resolver readback failed\n");
        CloseHandle(process);
        return 1;
    }
    std::printf("pid=%lu lua=0x%llX pre=%s pre_valid=1 resolver=%u "
                "post_valid=1 slots=13 current_module=1 result=0\n",
                pid, static_cast<unsigned long long>(lua.base),
                zero_prestate ? "zero" :
                    (current_prestate ? "current" : "partial"),
                1u);
    CloseHandle(process);
    return 0;
}
