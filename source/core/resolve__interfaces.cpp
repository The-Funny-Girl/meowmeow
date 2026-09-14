#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kMaterialSlot = 0x1E5DCC00000ull + 0x7EA818ull;
constexpr std::uint64_t kInputSlot = 0x1E5DCC00000ull + 0x7EA850ull;
constexpr std::uint64_t kClientSlot = 0x1E5DCC00000ull + 0x7EA810ull;
constexpr std::uint64_t kModelSlot = 0x1E5DCC00000ull + 0x7EA830ull;
constexpr std::uint64_t kVguiPanelSlot = 0x1E5DCC00000ull + 0x7EA858ull;
constexpr std::uint64_t kLuaSharedSlot = 0x1E5DCC00000ull + 0x7EA8C0ull;
constexpr std::uint64_t kClientStateSlot =
    0x1E5DCC00000ull + 0x7EA7E0ull;
constexpr std::uint64_t kPredictionEarlySlot =
    0x1E5DCC00000ull + 0x7EA7D8ull;
constexpr std::uint64_t kRenderViewSlot =
    0x1E5DCC00000ull + 0x7EA7F0ull;
constexpr std::uint64_t kStudioRenderSlot =
    0x1E5DCC00000ull + 0x7EA7F8ull;
constexpr std::uint64_t kViewRenderSlot =
    0x1E5DCC00000ull + 0x7EA800ull;
constexpr std::uint64_t kModelDuplicateSlot =
    0x1E5DCC00000ull + 0x7EA808ull;
constexpr std::uint64_t kNativeInputSlot =
    0x1E5DCC00000ull + 0x7EA820ull;
constexpr std::uint64_t kPredictionSeedSlot =
    0x1E5DCC00000ull + 0x7EA828ull;
constexpr std::uint64_t kEngineVguiSlot =
    0x1E5DCC00000ull + 0x7EA838ull;
constexpr std::uint64_t kGameEventSlot =
    0x1E5DCC00000ull + 0x7EA840ull;
constexpr std::uint64_t kServerHostnameSlot =
    0x1E5DCC00000ull + 0x7EA848ull;
constexpr std::uint64_t kMoveHelperSlot =
    0x1E5DCC00000ull + 0x7EA860ull;
constexpr std::uint64_t kGameMovementSlot =
    0x1E5DCC00000ull + 0x7EA868ull;
constexpr std::uint64_t kClientModeSlot =
    0x1E5DCC00000ull + 0x7EA878ull;
constexpr std::uint64_t kModelInfoSlot =
    0x1E5DCC00000ull + 0x7EA880ull;
constexpr std::uint64_t kVguiSurfaceSlot =
    0x1E5DCC00000ull + 0x7EA890ull;
constexpr std::uint64_t kEngineTraceSlot =
    0x1E5DCC00000ull + 0x7EA898ull;
constexpr std::uint64_t kClientEntityListSlot =
    0x1E5DCC00000ull + 0x7EA8A0ull;
constexpr std::uint64_t kCvarSlot = 0x1E5DCC00000ull + 0x7EA888ull;
constexpr std::uint64_t kEngineSlot = 0x1E5DCC00000ull + 0x7EA8A8ull;
constexpr std::uint64_t kGlobalsSlot = 0x1E5DCC00000ull + 0x7EA8B8ull;
constexpr std::uint64_t kEngineCodeSlot = 0x1E5DCC00000ull + 0x7EA7E8ull;
constexpr std::size_t kSendPacketCaveSize = 15;
constexpr std::uint64_t kSendPacketCaveDistance = 0x6F;
constexpr std::array<std::uint8_t, 3> kSendPacketInitializer{
    0x40, 0xB7, 0x01};
constexpr std::array<std::uint8_t, 3> kSendPacketRedirect{
    0xEB, 0x8F, 0x90};
constexpr std::array<std::uint8_t, 16> kSendPacketFunctionPrefix{
    0x40, 0x55, 0x53, 0x48, 0x8D, 0xAC, 0x24, 0x38,
    0xF0, 0xFF, 0xFF, 0xB8, 0xC8, 0x10, 0x00, 0x00};
constexpr DWORD kProcessSuspendResume = 0x0800;

struct ModuleInfo {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::string path;
};

struct CallContext {
    std::uint64_t material_factory;
    std::uint64_t input_factory;
    std::uint64_t engine_factory;
    std::uint64_t client_factory;
    std::uint64_t material_name;
    std::uint64_t input_name;
    std::uint64_t engine_name;
    std::uint64_t client_name;
    std::uint64_t material_result;
    std::uint64_t input_result;
    std::uint64_t engine_result;
    std::uint64_t client_result;
    char names[96];
};

class RemoteProcessSuspension {
  public:
    explicit RemoteProcessSuspension(HANDLE process) : process_(process) {}

    RemoteProcessSuspension(const RemoteProcessSuspension&) = delete;
    RemoteProcessSuspension& operator=(const RemoteProcessSuspension&) = delete;

    ~RemoteProcessSuspension() {
        if (suspended_)
            Resume();
    }

    bool Suspend() {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return false;
        suspend_ = reinterpret_cast<NtProcessRoutine>(
            GetProcAddress(ntdll, "NtSuspendProcess"));
        resume_ = reinterpret_cast<NtProcessRoutine>(
            GetProcAddress(ntdll, "NtResumeProcess"));
        if (suspend_ && resume_) {
            SetLastError(ERROR_SUCCESS);
            const LONG status = suspend_(process_);
            if (status >= 0) {
                suspended_ = true;
                native_ = true;
                return true;
            }
        }
        return SuspendThreads();
    }

    bool Resume() {
        if (!suspended_)
            return true;
        bool ok = true;
        if (native_) {
            if (!resume_)
                return false;
            SetLastError(ERROR_SUCCESS);
            const LONG status = resume_(process_);
            if (status < 0) {
                SetLastError(static_cast<DWORD>(status));
                return false;
            }
        } else {
            for (auto iterator = thread_handles_.rbegin();
                 iterator != thread_handles_.rend(); ++iterator) {
                if (ResumeThread(*iterator) == static_cast<DWORD>(-1))
                    ok = false;
                CloseHandle(*iterator);
            }
            thread_handles_.clear();
        }
        suspended_ = false;
        return ok;
    }

  private:
    using NtProcessRoutine = LONG(NTAPI*)(HANDLE);

    bool SuspendThreads() {
        const DWORD pid = GetProcessId(process_);
        if (!pid)
            return false;
        HANDLE snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        bool ok = Thread32First(snapshot, &entry) != FALSE;
        while (ok) {
            if (entry.th32OwnerProcessID == pid) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME |
                                               THREAD_QUERY_LIMITED_INFORMATION,
                                           FALSE, entry.th32ThreadID);
                if (!thread ||
                    SuspendThread(thread) == static_cast<DWORD>(-1)) {
                    if (thread)
                        CloseHandle(thread);
                    CloseHandle(snapshot);
                    for (auto iterator = thread_handles_.rbegin();
                         iterator != thread_handles_.rend(); ++iterator) {
                        ResumeThread(*iterator);
                        CloseHandle(*iterator);
                    }
                    thread_handles_.clear();
                    return false;
                }
                thread_handles_.push_back(thread);
            }
            ok = Thread32Next(snapshot, &entry) != FALSE;
        }
        CloseHandle(snapshot);
        if (thread_handles_.empty())
            return false;
        suspended_ = true;
        native_ = false;
        return true;
    }

    HANDLE process_ = nullptr;
    NtProcessRoutine suspend_ = nullptr;
    NtProcessRoutine resume_ = nullptr;
    std::vector<HANDLE> thread_handles_;
    bool suspended_ = false;
    bool native_ = false;
};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

ModuleInfo FindModule(DWORD pid, const char* name) {
    ModuleInfo result;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            if (Lower(entry.szModule) == Lower(name)) {
                result.base = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
                result.size = entry.modBaseSize;
                result.path = entry.szExePath;
                break;
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::uint64_t ExportRva(const ModuleInfo& module, const char* name) {
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return 0;
    const auto symbol = GetProcAddress(local, name);
    const auto rva = symbol
                         ? reinterpret_cast<std::uint64_t>(symbol) -
                               reinterpret_cast<std::uint64_t>(local)
                         : 0;
    FreeLibrary(local);
    return rva;
}

std::uint64_t ResolveEngineGlobalsRva(const ModuleInfo& module) {
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return 0;
    const auto* base = reinterpret_cast<const std::uint8_t*>(local);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        FreeLibrary(local);
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(local);
        return 0;
    }
    static constexpr std::array<int, 42> pattern{
        0x48, 0x8B, 0x0D, -1, -1, -1, -1,
        0x4C, 0x8D, 0x0D, -1, -1, -1, -1,
        0x48, 0x8B, 0x15, -1, -1, -1, -1,
        0x48, 0x8B, 0x05, -1, -1, -1, -1,
        0x4C, 0x8B, 0xC2, 0x48, 0x89, 0x44, 0x24, 0x20,
        0x4C, 0x8B, 0x11, 0x41, 0xFF, 0x12};
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* match = nullptr;
    std::size_t matches = 0;
    for (WORD section_index = 0;
         section_index < nt->FileHeader.NumberOfSections;
         ++section_index) {
        const auto& section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const std::size_t size = section.Misc.VirtualSize;
        if (size < pattern.size())
            continue;
        const auto* bytes = base + section.VirtualAddress;
        for (std::size_t offset = 0; offset + pattern.size() <= size;
             ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index < pattern.size(); ++index) {
                if (pattern[index] >= 0 &&
                    bytes[offset + index] !=
                        static_cast<std::uint8_t>(pattern[index])) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            match = bytes + offset;
            ++matches;
        }
    }
    std::uint64_t rva = 0;
    if (matches == 1 && match) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, match + 10, sizeof(displacement));
        const auto instruction_rva =
            static_cast<std::uint64_t>(match - base);
        const auto target = static_cast<std::int64_t>(instruction_rva + 14) +
                            static_cast<std::int64_t>(displacement);
        if (target > 0 &&
            static_cast<std::uint64_t>(target) + 0x3C <=
                nt->OptionalHeader.SizeOfImage)
            rva = static_cast<std::uint64_t>(target);
    }
    FreeLibrary(local);
    return rva;
}

bool WriteQword(HANDLE process, std::uint64_t address, std::uint64_t value) {
    DWORD prior = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address),
                          sizeof(value), PAGE_READWRITE, &prior))
        return false;
    SIZE_T done = 0;
    const bool wrote =
        WriteProcessMemory(process, reinterpret_cast<void*>(address), &value,
                           sizeof(value), &done) != FALSE &&
        done == sizeof(value);
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(address),
                         sizeof(value), prior, &ignored) != FALSE;
    return wrote && restored;
}

bool ReadQword(HANDLE process, std::uint64_t address, std::uint64_t& value) {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             &value, sizeof(value), &done) != FALSE &&
           done == sizeof(value);
}

bool ReadByte(HANDLE process, std::uint64_t address, std::uint8_t& value) {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             &value, sizeof(value), &done) != FALSE &&
           done == sizeof(value);
}

bool ReadBytes(HANDLE process, std::uint64_t address, void* bytes,
               SIZE_T size) {
    SIZE_T done = 0;
    return size != 0 &&
           ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             bytes, size, &done) != FALSE &&
           done == size;
}

bool WriteBytes(HANDLE process, std::uint64_t address, const void* bytes,
                SIZE_T size) {
    SIZE_T done = 0;
    return size != 0 &&
           WriteProcessMemory(process, reinterpret_cast<void*>(address),
                              bytes, size, &done) != FALSE &&
           done == size;
}

bool IsCommittedWritableNonExecutable(HANDLE process, std::uint64_t address,
                                      SIZE_T size) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!size || VirtualQueryEx(process,
                                reinterpret_cast<const void*>(address),
                                &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const DWORD access = memory.Protect & 0xFFu;
    if (access != PAGE_READWRITE && access != PAGE_WRITECOPY)
        return false;
    const auto region = reinterpret_cast<std::uint64_t>(memory.BaseAddress);
    return address >= region && address - region <= memory.RegionSize &&
           size <= memory.RegionSize - static_cast<SIZE_T>(address - region);
}

bool InstallSendPacketBridge(HANDLE process, const ModuleInfo& engine,
                             std::uint64_t initializer_rva,
                             std::uint64_t storage_slot,
                             std::uint64_t& shadow_address,
                             std::uint64_t& cave_address,
                             DWORD& restored_protection,
                             std::uint64_t& original_slot_value,
                             int& failure_stage) {
    shadow_address = 0;
    cave_address = 0;
    restored_protection = 0;
    original_slot_value = 0;
    failure_stage = 0;
    if (!process || !engine.base || !storage_slot ||
        initializer_rva < kSendPacketCaveDistance ||
        initializer_rva + kSendPacketInitializer.size() > engine.size)
        return failure_stage = 1, false;
    const std::uint64_t initializer = engine.base + initializer_rva;
    const std::uint64_t cave = initializer - kSendPacketCaveDistance;
    const std::uint64_t function = initializer - 0x60;
    std::array<std::uint8_t, kSendPacketCaveSize> cave_original{};
    std::array<std::uint8_t, 3> initializer_original{};
    std::array<std::uint8_t, kSendPacketFunctionPrefix.size()>
        function_prefix{};
    if (!ReadBytes(process, cave, cave_original.data(),
                   cave_original.size()) ||
        !ReadBytes(process, initializer, initializer_original.data(),
                   initializer_original.size()) ||
        !ReadBytes(process, function, function_prefix.data(),
                   function_prefix.size()) ||
        !std::all_of(cave_original.begin(), cave_original.end(),
                     [](std::uint8_t value) { return value == 0xCC; }) ||
        initializer_original != kSendPacketInitializer ||
        function_prefix != kSendPacketFunctionPrefix)
        return failure_stage = 2, false;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T page_size = static_cast<SIZE_T>(system_info.dwPageSize);
    if (!page_size)
        return failure_stage = 3, false;
    const std::uint64_t page =
        cave - cave % static_cast<std::uint64_t>(page_size);
    if (initializer + kSendPacketInitializer.size() >
        page + page_size)
        return failure_stage = 4, false;
    void* shadow = VirtualAllocEx(process, nullptr, page_size,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!shadow)
        return failure_stage = 5, false;
    const std::uint64_t shadow_value =
        reinterpret_cast<std::uint64_t>(shadow);
    const std::uint8_t initial_value = 1;
    std::array<std::uint8_t, kSendPacketCaveSize> bridge{
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x40, 0x8A, 0x38, 0xEB, 0x63};
    std::memcpy(bridge.data() + 2, &shadow_value, sizeof(shadow_value));
    std::uint64_t slot_original = 0;
    if (!ReadBytes(process, storage_slot, &slot_original,
                   sizeof(slot_original))) {
        VirtualFreeEx(process, shadow, 0, MEM_RELEASE);
        return failure_stage = 6, false;
    }
    RemoteProcessSuspension suspension(process);
    if (!suspension.Suspend()) {
        VirtualFreeEx(process, shadow, 0, MEM_RELEASE);
        return failure_stage = 7, false;
    }
    if (!ReadBytes(process, cave, cave_original.data(),
                   cave_original.size()) ||
        !ReadBytes(process, initializer, initializer_original.data(),
                   initializer_original.size()) ||
        !ReadBytes(process, function, function_prefix.data(),
                   function_prefix.size()) ||
        !std::all_of(cave_original.begin(), cave_original.end(),
                     [](std::uint8_t value) { return value == 0xCC; }) ||
        initializer_original != kSendPacketInitializer ||
        function_prefix != kSendPacketFunctionPrefix) {
        VirtualFreeEx(process, shadow, 0, MEM_RELEASE);
        return failure_stage = 8, false;
    }
    bool initializer_changed = false;
    bool cave_changed = false;
    bool slot_changed = false;
    DWORD previous = 0;
    bool protected_for_write = false;
    auto rollback = [&]() {
        bool restored = true;
        if (slot_changed)
            restored = WriteQword(process, storage_slot, slot_original) &&
                       restored;
        if (!protected_for_write &&
            (initializer_changed || cave_changed)) {
            DWORD rollback_previous = 0;
            if (VirtualProtectEx(process, reinterpret_cast<void*>(page),
                                 page_size, PAGE_EXECUTE_READWRITE,
                                 &rollback_previous)) {
                previous = rollback_previous;
                protected_for_write = true;
            } else {
                restored = false;
            }
        }
        if (protected_for_write) {
            if (initializer_changed)
                restored = WriteBytes(
                               process, initializer,
                               kSendPacketInitializer.data(),
                               kSendPacketInitializer.size()) &&
                           restored;
            if (cave_changed)
                restored = WriteBytes(process, cave,
                                      cave_original.data(),
                                      cave_original.size()) &&
                           restored;
            FlushInstructionCache(process,
                                  reinterpret_cast<const void*>(cave),
                                  initializer +
                                          kSendPacketInitializer.size() -
                                      cave);
            DWORD ignored = 0;
            restored =
                VirtualProtectEx(process, reinterpret_cast<void*>(page),
                                 page_size, previous, &ignored) != FALSE &&
                restored;
        }
        VirtualFreeEx(process, shadow, 0, MEM_RELEASE);
        return restored;
    };
    if (!WriteBytes(process, shadow_value, &initial_value,
                    sizeof(initial_value))) {
        rollback();
        return failure_stage = 9, false;
    }
    if (!IsCommittedWritableNonExecutable(process, shadow_value, 1)) {
        rollback();
        return failure_stage = 10, false;
    }
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(page), page_size,
                          PAGE_EXECUTE_READWRITE, &previous)) {
        rollback();
        return failure_stage = 11, false;
    }
    protected_for_write = true;
    if (!WriteBytes(process, cave, bridge.data(), bridge.size())) {
        rollback();
        return failure_stage = 12, false;
    }
    cave_changed = true;
    if (!FlushInstructionCache(process, reinterpret_cast<const void*>(cave),
                               bridge.size())) {
        rollback();
        return failure_stage = 13, false;
    }
    if (!WriteBytes(process, initializer, kSendPacketRedirect.data(),
                    kSendPacketRedirect.size())) {
        rollback();
        return failure_stage = 14, false;
    }
    initializer_changed = true;
    if (!FlushInstructionCache(
            process, reinterpret_cast<const void*>(initializer),
            kSendPacketRedirect.size())) {
        rollback();
        return failure_stage = 15, false;
    }
    DWORD ignored = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(page), page_size,
                          previous, &ignored)) {
        rollback();
        return failure_stage = 16, false;
    }
    protected_for_write = false;
    std::array<std::uint8_t, kSendPacketCaveSize> bridge_verified{};
    std::array<std::uint8_t, 3> redirect_verified{};
    std::uint8_t shadow_verified = 0;
    MEMORY_BASIC_INFORMATION engine_memory{};
    const bool exact =
        ReadBytes(process, cave, bridge_verified.data(),
                  bridge_verified.size()) &&
        bridge_verified == bridge &&
        ReadBytes(process, initializer, redirect_verified.data(),
                  redirect_verified.size()) &&
        redirect_verified == kSendPacketRedirect &&
        ReadByte(process, shadow_value, shadow_verified) &&
        shadow_verified == initial_value &&
        IsCommittedWritableNonExecutable(process, shadow_value, 1) &&
        VirtualQueryEx(process,
                       reinterpret_cast<const void*>(initializer),
                       &engine_memory,
                       sizeof(engine_memory)) == sizeof(engine_memory) &&
        (engine_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
        (engine_memory.Protect & 0xFFu) != PAGE_EXECUTE_READWRITE &&
        (engine_memory.Protect & 0xFFu) != PAGE_EXECUTE_WRITECOPY;
    if (!exact) {
        rollback();
        return failure_stage = 17, false;
    }
    if (!WriteQword(process, storage_slot, shadow_value)) {
        rollback();
        return failure_stage = 18, false;
    }
    slot_changed = true;
    std::uint64_t slot_verified = 0;
    if (!ReadBytes(process, storage_slot, &slot_verified,
                   sizeof(slot_verified)) ||
        slot_verified != shadow_value) {
        rollback();
        return failure_stage = 19, false;
    }
    if (!suspension.Resume()) {
        rollback();
        suspension.Resume();
        return failure_stage = 20, false;
    }
    shadow_address = shadow_value;
    cave_address = cave;
    restored_protection = previous;
    original_slot_value = slot_original;
    failure_stage = 100;
    return true;
}

bool RemoveSendPacketBridge(HANDLE process, const ModuleInfo& engine,
                            std::uint64_t initializer_rva,
                            std::uint64_t storage_slot,
                            std::uint64_t shadow_address,
                            std::uint64_t original_slot_value) {
    if (!process || !engine.base || !shadow_address || !storage_slot ||
        initializer_rva < kSendPacketCaveDistance ||
        initializer_rva + kSendPacketInitializer.size() > engine.size)
        return false;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T page_size = static_cast<SIZE_T>(system_info.dwPageSize);
    if (!page_size)
        return false;
    const std::uint64_t initializer = engine.base + initializer_rva;
    const std::uint64_t cave = initializer - kSendPacketCaveDistance;
    const std::uint64_t page =
        cave - cave % static_cast<std::uint64_t>(page_size);
    RemoteProcessSuspension suspension(process);
    if (!suspension.Suspend())
        return false;
    DWORD previous = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(page), page_size,
                          PAGE_EXECUTE_READWRITE, &previous))
        return false;
    std::array<std::uint8_t, kSendPacketCaveSize> cave_original{};
    cave_original.fill(0xCC);
    const bool initializer_restored =
        WriteBytes(process, initializer, kSendPacketInitializer.data(),
                   kSendPacketInitializer.size());
    const bool cave_restored =
        initializer_restored &&
        WriteBytes(process, cave, cave_original.data(), cave_original.size());
    const bool cache_flushed =
        cave_restored &&
        FlushInstructionCache(process, reinterpret_cast<const void*>(cave),
                              initializer + kSendPacketInitializer.size() -
                                  cave) != FALSE;
    DWORD ignored = 0;
    const bool protection_restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(page), page_size,
                         previous, &ignored) != FALSE;
    const bool slot_restored =
        cache_flushed && protection_restored &&
        WriteQword(process, storage_slot, original_slot_value);
    std::array<std::uint8_t, 3> initializer_verified{};
    std::array<std::uint8_t, kSendPacketCaveSize> cave_verified{};
    const bool exact =
        slot_restored &&
        ReadBytes(process, initializer, initializer_verified.data(),
                  initializer_verified.size()) &&
        initializer_verified == kSendPacketInitializer &&
        ReadBytes(process, cave, cave_verified.data(),
                  cave_verified.size()) &&
        cave_verified == cave_original;
    const bool freed =
        exact && VirtualFreeEx(process,
                               reinterpret_cast<void*>(shadow_address), 0,
                               MEM_RELEASE) != FALSE;
    return freed && suspension.Resume();
}

bool VerifySendPacketBridge(HANDLE process, const ModuleInfo& engine,
                            std::uint64_t initializer_rva,
                            std::uint64_t storage_slot,
                            std::uint64_t shadow_address) {
    if (!process || !engine.base || !storage_slot || !shadow_address ||
        initializer_rva < kSendPacketCaveDistance ||
        initializer_rva + kSendPacketRedirect.size() > engine.size)
        return false;
    const std::uint64_t initializer = engine.base + initializer_rva;
    const std::uint64_t cave = initializer - kSendPacketCaveDistance;
    std::array<std::uint8_t, kSendPacketCaveSize> expected_bridge{
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x40, 0x8A, 0x38, 0xEB, 0x63};
    std::memcpy(expected_bridge.data() + 2, &shadow_address,
                sizeof(shadow_address));
    std::array<std::uint8_t, kSendPacketCaveSize> actual_bridge{};
    std::array<std::uint8_t, kSendPacketRedirect.size()> actual_redirect{};
    std::uint64_t actual_slot = 0;
    MEMORY_BASIC_INFORMATION engine_memory{};
    return ReadBytes(process, cave, actual_bridge.data(),
                     actual_bridge.size()) &&
           actual_bridge == expected_bridge &&
           ReadBytes(process, initializer, actual_redirect.data(),
                     actual_redirect.size()) &&
           actual_redirect == kSendPacketRedirect &&
           ReadQword(process, storage_slot, actual_slot) &&
           actual_slot == shadow_address &&
           IsCommittedWritableNonExecutable(process, shadow_address, 1) &&
           VirtualQueryEx(process,
                          reinterpret_cast<const void*>(initializer),
                          &engine_memory,
                          sizeof(engine_memory)) == sizeof(engine_memory) &&
           engine_memory.State == MEM_COMMIT &&
           engine_memory.Type == MEM_IMAGE &&
           reinterpret_cast<std::uint64_t>(engine_memory.AllocationBase) ==
               engine.base &&
           (engine_memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
           (engine_memory.Protect & 0xFFu) != PAGE_EXECUTE_READWRITE &&
           (engine_memory.Protect & 0xFFu) != PAGE_EXECUTE_WRITECOPY &&
           ((engine_memory.Protect & 0xFFu) == PAGE_EXECUTE ||
            (engine_memory.Protect & 0xFFu) == PAGE_EXECUTE_READ);
}

template <typename T>
bool ReadValue(HANDLE process, std::uint64_t address, T& value) {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             &value, sizeof(value), &done) != FALSE &&
           done == sizeof(value);
}

bool IsCommittedReadable(HANDLE process, std::uint64_t address,
                         SIZE_T size) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!size || VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                                &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const DWORD access = memory.Protect & 0xFFu;
    if (access != PAGE_READONLY && access != PAGE_READWRITE &&
        access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
        access != PAGE_EXECUTE_READWRITE &&
        access != PAGE_EXECUTE_WRITECOPY)
        return false;
    const auto region = reinterpret_cast<std::uint64_t>(memory.BaseAddress);
    return address >= region && address - region <= memory.RegionSize &&
           size <= memory.RegionSize - static_cast<SIZE_T>(address - region);
}

bool IsCommittedExecutable(HANDLE process, std::uint64_t address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                       &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const DWORD access = memory.Protect & 0xFFu;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool ResolveInterfaceObject(HANDLE process, const ModuleInfo& module,
                            const char* name, std::uint64_t& result) {
    result = 0;
    const auto factory_rva = ExportRva(module, "CreateInterface");
    if (!process || !module.base || !factory_rva || !name ||
        std::strlen(name) >= sizeof(CallContext::names))
        return false;
    static constexpr std::array<std::uint8_t, 29> stub{
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB,
        0x48, 0x8B, 0x03, 0x48, 0x8B, 0x4B, 0x20, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x40, 0x48,
        0x83, 0xC4, 0x20, 0x5B, 0xC3};
    const SIZE_T context_size =
        (sizeof(CallContext) + 15u) & ~SIZE_T(15u);
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, context_size + stub.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote)
        return false;
    CallContext context{};
    context.material_factory = module.base + factory_rva;
    context.material_name = reinterpret_cast<std::uint64_t>(remote) +
                            offsetof(CallContext, names);
    std::strcpy(context.names, name);
    SIZE_T done = 0;
    bool ok =
        WriteProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context) &&
        WriteProcessMemory(process, remote + context_size, stub.data(),
                           stub.size(), &done) != FALSE &&
        done == stub.size();
    ok = ok && FlushInstructionCache(process, remote + context_size,
                                     stub.size()) != FALSE;
    HANDLE thread =
        ok ? CreateRemoteThread(
                 process, nullptr, 0,
                 reinterpret_cast<LPTHREAD_START_ROUTINE>(
                     remote + context_size),
                 remote, 0, nullptr)
           : nullptr;
    const DWORD wait =
        thread ? WaitForSingleObject(thread, 30000) : WAIT_FAILED;
    done = 0;
    ok = wait == WAIT_OBJECT_0 &&
         ReadProcessMemory(process, remote, &context, sizeof(context), &done) !=
             FALSE &&
         done == sizeof(context);
    if (thread)
        CloseHandle(thread);
    if (wait == WAIT_OBJECT_0)
        ok = VirtualFreeEx(process, remote, 0, MEM_RELEASE) != FALSE && ok;
    if (!ok || !context.material_result)
        return false;
    std::uint64_t table = 0;
    if (!ReadQword(process, context.material_result, table) ||
        table < module.base || table >= module.base + module.size ||
        !IsCommittedReadable(process, table, sizeof(std::uint64_t)))
        return false;
    result = context.material_result;
    return true;
}

template <std::size_t Size>
std::uint64_t ResolveUniqueRipRva(
    const ModuleInfo& module, const std::array<int, Size>& pattern,
    std::size_t displacement_offset, std::size_t instruction_end_offset) {
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local || displacement_offset + sizeof(std::int32_t) > Size ||
        instruction_end_offset > Size) {
        if (local)
            FreeLibrary(local);
        return 0;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(local);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE
                         ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                               base + dos->e_lfanew)
                         : nullptr;
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(local);
        return 0;
    }
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* match = nullptr;
    std::size_t matches = 0;
    for (WORD section_index = 0;
         section_index < nt->FileHeader.NumberOfSections;
         ++section_index) {
        const auto& section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.Misc.VirtualSize < Size)
            continue;
        const auto* bytes = base + section.VirtualAddress;
        for (std::size_t offset = 0;
             offset + Size <= section.Misc.VirtualSize; ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index != Size; ++index) {
                if (pattern[index] >= 0 &&
                    bytes[offset + index] !=
                        static_cast<std::uint8_t>(pattern[index])) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                match = bytes + offset;
                ++matches;
            }
        }
    }
    std::uint64_t target_rva = 0;
    if (matches == 1 && match) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, match + displacement_offset,
                    sizeof(displacement));
        const auto match_rva = static_cast<std::uint64_t>(match - base);
        const auto target =
            static_cast<std::int64_t>(match_rva + instruction_end_offset) +
            static_cast<std::int64_t>(displacement);
        if (target > 0 &&
            static_cast<std::uint64_t>(target) <
                nt->OptionalHeader.SizeOfImage)
            target_rva = static_cast<std::uint64_t>(target);
    }
    FreeLibrary(local);
    return target_rva;
}

template <std::size_t Size>
std::uint64_t ResolveUniquePatternRva(
    const ModuleInfo& module, const std::array<int, Size>& pattern,
    std::size_t result_offset) {
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local || result_offset >= Size) {
        if (local)
            FreeLibrary(local);
        return 0;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(local);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE
                         ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                               base + dos->e_lfanew)
                         : nullptr;
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(local);
        return 0;
    }
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* match = nullptr;
    std::size_t matches = 0;
    for (WORD section_index = 0;
         section_index < nt->FileHeader.NumberOfSections; ++section_index) {
        const auto& section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.Misc.VirtualSize < Size)
            continue;
        const auto* bytes = base + section.VirtualAddress;
        for (std::size_t offset = 0;
             offset + Size <= section.Misc.VirtualSize; ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index != Size; ++index) {
                if (pattern[index] >= 0 &&
                    bytes[offset + index] !=
                        static_cast<std::uint8_t>(pattern[index])) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            match = bytes + offset;
            ++matches;
        }
    }
    std::uint64_t rva = 0;
    if (matches == 1 && match) {
        const auto candidate =
            static_cast<std::uint64_t>(match - base) + result_offset;
        if (candidate < nt->OptionalHeader.SizeOfImage)
            rva = candidate;
    }
    FreeLibrary(local);
    return rva;
}

bool ResolveMoveHelperRvas(const ModuleInfo& module,
                           std::uint64_t& object_rva,
                           std::uint64_t& pointer_cell_rva) {
    object_rva = 0;
    pointer_cell_rva = 0;
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return false;
    const auto* base = reinterpret_cast<const std::uint8_t*>(local);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE
                         ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                               base + dos->e_lfanew)
                         : nullptr;
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(local);
        return false;
    }

    static constexpr auto pattern = std::to_array<int>({
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0x48, 0x89, 0x05, -1, -1, -1, -1,
        0xE9, -1, -1, -1, -1});
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const auto section_contains =
        [&](std::uint64_t rva, std::size_t range_size, DWORD required,
            DWORD forbidden) {
            for (WORD index = 0; index != nt->FileHeader.NumberOfSections;
                 ++index) {
                const auto& section = sections[index];
                const std::uint64_t begin = section.VirtualAddress;
                const std::uint64_t extent = section.Misc.VirtualSize;
                if ((section.Characteristics & required) != required ||
                    (section.Characteristics & forbidden) != 0 ||
                    rva < begin || rva - begin > extent ||
                    range_size > extent - (rva - begin))
                    continue;
                return true;
            }
            return false;
        };
    std::size_t matches = 0;
    for (WORD section_index = 0;
         section_index != nt->FileHeader.NumberOfSections; ++section_index) {
        const auto& section = sections[section_index];
        const std::size_t size = section.Misc.VirtualSize;
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            size < pattern.size())
            continue;
        const auto* bytes = base + section.VirtualAddress;
        for (std::size_t offset = 0; offset + pattern.size() <= size;
             ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index != pattern.size(); ++index) {
                if (pattern[index] >= 0 &&
                    bytes[offset + index] !=
                        static_cast<std::uint8_t>(pattern[index])) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            const std::uint64_t match_rva =
                section.VirtualAddress + offset;
            std::int32_t object_displacement = 0;
            std::int32_t destructor_displacement = 0;
            std::int32_t cell_displacement = 0;
            std::memcpy(&object_displacement, bytes + offset + 3,
                        sizeof(object_displacement));
            std::memcpy(&destructor_displacement, bytes + offset + 10,
                        sizeof(destructor_displacement));
            std::memcpy(&cell_displacement, bytes + offset + 17,
                        sizeof(cell_displacement));
            const std::int64_t object =
                static_cast<std::int64_t>(match_rva + 7) +
                object_displacement;
            const std::int64_t destructor =
                static_cast<std::int64_t>(match_rva + 14) +
                destructor_displacement;
            const std::int64_t cell =
                static_cast<std::int64_t>(match_rva + 21) +
                cell_displacement;
            if (object <= 0 || destructor <= 0 || cell <= 0)
                continue;
            const auto candidate_object =
                static_cast<std::uint64_t>(object);
            const auto candidate_destructor =
                static_cast<std::uint64_t>(destructor);
            const auto candidate_cell = static_cast<std::uint64_t>(cell);
            if (!section_contains(candidate_object, sizeof(std::uint64_t),
                                  IMAGE_SCN_MEM_WRITE,
                                  IMAGE_SCN_MEM_EXECUTE) ||
                !section_contains(candidate_destructor, 1,
                                  IMAGE_SCN_MEM_EXECUTE, 0) ||
                !section_contains(candidate_cell, sizeof(std::uint64_t),
                                  IMAGE_SCN_MEM_WRITE,
                                  IMAGE_SCN_MEM_EXECUTE))
                continue;
            object_rva = candidate_object;
            pointer_cell_rva = candidate_cell;
            ++matches;
        }
    }
    FreeLibrary(local);
    if (matches != 1) {
        object_rva = 0;
        pointer_cell_rva = 0;
        return false;
    }
    return true;
}

bool ResolveMoveHelper(HANDLE process, const ModuleInfo& client,
                       std::uint64_t object_rva,
                       std::uint64_t pointer_cell_rva,
                       std::uint64_t& move_helper) {
    move_helper = 0;
    if (!process || !client.base || object_rva > client.size ||
        sizeof(std::uint64_t) > client.size - object_rva ||
        pointer_cell_rva > client.size ||
        sizeof(std::uint64_t) > client.size - pointer_cell_rva)
        return false;
    const std::uint64_t expected = client.base + object_rva;
    const std::uint64_t pointer_cell = client.base + pointer_cell_rva;
    std::uint64_t vtable = 0;
    std::uint64_t set_host = 0;
    if (!IsCommittedWritableNonExecutable(
            process, pointer_cell, sizeof(std::uint64_t)) ||
        !IsCommittedWritableNonExecutable(
            process, expected, sizeof(std::uint64_t)) ||
        !ReadQword(process, pointer_cell, move_helper) ||
        move_helper != expected ||
        !ReadQword(process, move_helper, vtable) ||
        vtable < client.base || vtable >= client.base + client.size ||
        !IsCommittedReadable(process, vtable, 2 * sizeof(std::uint64_t)) ||
        !ReadQword(process, vtable + sizeof(std::uint64_t), set_host) ||
        set_host < client.base || set_host >= client.base + client.size ||
        !IsCommittedExecutable(process, set_host)) {
        move_helper = 0;
        return false;
    }
    return true;
}

bool ResolveNativeInput(HANDLE process, const ModuleInfo& client,
                        std::uint64_t client_object,
                        std::uint64_t& native_input) {
    native_input = 0;
    std::uint64_t table = 0;
    std::uint64_t function = 0;
    std::array<std::uint8_t, 14> bytes{};
    SIZE_T done = 0;
    if (!ReadQword(process, client_object, table) ||
        !ReadQword(process, table + 20 * sizeof(std::uint64_t), function) ||
        function < client.base || function >= client.base + client.size ||
        !ReadProcessMemory(process, reinterpret_cast<const void*>(function),
                           bytes.data(), bytes.size(), &done) ||
        done != bytes.size())
        return false;
    static constexpr std::array<std::uint8_t, 10> fixed{
        0x48, 0x8B, 0x0D, 0x48, 0x8B,
        0x01, 0x48, 0xFF, 0x60, 0x58};
    if (bytes[0] != fixed[0] || bytes[1] != fixed[1] ||
        bytes[2] != fixed[2] || bytes[7] != fixed[3] ||
        bytes[8] != fixed[4] || bytes[9] != fixed[5] ||
        bytes[10] != fixed[6] || bytes[11] != fixed[7] ||
        bytes[12] != fixed[8] || bytes[13] != fixed[9])
        return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
    const auto cell_signed = static_cast<std::int64_t>(function + 7) +
                             static_cast<std::int64_t>(displacement);
    if (cell_signed <= 0)
        return false;
    const auto cell = static_cast<std::uint64_t>(cell_signed);
    if (cell < client.base || cell + sizeof(std::uint64_t) >
                                  client.base + client.size ||
        !ReadQword(process, cell, native_input) ||
        native_input < client.base ||
        native_input >= client.base + client.size ||
        !IsCommittedReadable(process, native_input, 0xE0))
        return false;
    return true;
}

std::uint64_t ResolvePredictionSeedRva(const ModuleInfo& module) {
    HMODULE local = LoadLibraryExA(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return 0;
    const auto* base = reinterpret_cast<const std::uint8_t*>(local);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE
                         ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                               base + dos->e_lfanew)
                         : nullptr;
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(local);
        return 0;
    }
    static constexpr auto setter_pattern = std::to_array<int>({
        0x48, 0x85, 0xC9, 0x75, 0x0B,
        0xC7, 0x05, -1, -1, -1, -1,
        0xFF, 0xFF, 0xFF, 0xFF, 0xC3,
        0x8B, 0x41, 0x2C,
        0x89, 0x05, -1, -1, -1, -1, 0xC3,
    });
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* match = nullptr;
    std::size_t matches = 0;
    for (WORD section_index = 0;
         section_index < nt->FileHeader.NumberOfSections; ++section_index) {
        const auto& section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.Misc.VirtualSize < setter_pattern.size())
            continue;
        const auto* bytes = base + section.VirtualAddress;
        for (std::size_t offset = 0;
             offset + setter_pattern.size() <= section.Misc.VirtualSize;
             ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index != setter_pattern.size();
                 ++index) {
                if (setter_pattern[index] >= 0 &&
                    bytes[offset + index] != static_cast<std::uint8_t>(
                                                   setter_pattern[index])) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            match = bytes + offset;
            ++matches;
        }
    }
    std::uint64_t rva = 0;
    if (matches == 1 && match) {
        std::int32_t null_displacement = 0;
        std::int32_t live_displacement = 0;
        std::memcpy(&null_displacement, match + 7,
                    sizeof(null_displacement));
        std::memcpy(&live_displacement, match + 21,
                    sizeof(live_displacement));
        const auto match_rva = static_cast<std::uint64_t>(match - base);
        const auto null_target = static_cast<std::int64_t>(match_rva + 15) +
                                 static_cast<std::int64_t>(null_displacement);
        const auto live_target = static_cast<std::int64_t>(match_rva + 25) +
                                 static_cast<std::int64_t>(live_displacement);
        if (null_target > 0 && null_target == live_target &&
            static_cast<std::uint64_t>(null_target) +
                    sizeof(std::int32_t) <=
                nt->OptionalHeader.SizeOfImage) {
            const auto candidate = static_cast<std::uint64_t>(null_target);
            bool writable_non_executable = false;
            for (WORD section_index = 0;
                 section_index < nt->FileHeader.NumberOfSections;
                 ++section_index) {
                const auto& section = sections[section_index];
                const auto section_begin =
                    static_cast<std::uint64_t>(section.VirtualAddress);
                const auto section_end = section_begin +
                    static_cast<std::uint64_t>(section.Misc.VirtualSize);
                if (candidate < section_begin ||
                    candidate + sizeof(std::int32_t) > section_end)
                    continue;
                writable_non_executable =
                    (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0 &&
                    (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0;
                break;
            }
            std::int32_t initial_seed = 0;
            std::memcpy(&initial_seed, base + candidate,
                        sizeof(initial_seed));
            if (writable_non_executable && initial_seed == -1)
                rva = candidate;
        }
    }
    FreeLibrary(local);
    return rva;
}

}

int ResolveSingleInterface(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const auto module = FindModule(pid, argv[2]);
    const auto factory_rva = ExportRva(module, "CreateInterface");
    if (!module.base || !factory_rva)
        return 1;
    constexpr DWORD process_access =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
    HANDLE process = OpenProcess(process_access | kProcessSuspendResume,
                                 FALSE, pid);
    if (!process)
        process = OpenProcess(process_access, FALSE, pid);
    if (!process)
        return 1;
    static constexpr std::array<std::uint8_t, 29> stub{
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB,
        0x48, 0x8B, 0x03, 0x48, 0x8B, 0x4B, 0x20, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x40, 0x48,
        0x83, 0xC4, 0x20, 0x5B, 0xC3};
    const SIZE_T context_size = (sizeof(CallContext) + 15u) & ~SIZE_T(15u);
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, context_size + stub.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote) {
        CloseHandle(process);
        return 1;
    }
    CallContext context{};
    context.material_factory = module.base + factory_rva;
    context.material_name = reinterpret_cast<std::uint64_t>(remote) +
                            offsetof(CallContext, names);
    if (std::strlen(argv[3]) >= sizeof(context.names)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    std::strcpy(context.names, argv[3]);
    SIZE_T done = 0;
    const bool wrote =
        WriteProcessMemory(process, remote, &context, sizeof(context), &done) &&
        done == sizeof(context) &&
        WriteProcessMemory(process, remote + context_size, stub.data(),
                           stub.size(), &done) &&
        done == stub.size();
    HANDLE thread =
        wrote ? CreateRemoteThread(
                    process, nullptr, 0,
                    reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + context_size),
                    remote, 0, nullptr)
              : nullptr;
    const DWORD wait = thread ? WaitForSingleObject(thread, 30000) : WAIT_FAILED;
    done = 0;
    const bool read =
        wait == WAIT_OBJECT_0 &&
        ReadProcessMemory(process, remote, &context, sizeof(context), &done) &&
        done == sizeof(context);
    std::uint64_t table = 0;
    const bool valid =
        read && context.material_result &&
        ReadQword(process, context.material_result, table) && table;
    std::printf(
        "%s|%s|base=0x%llX|size=0x%llX|object=0x%llX|rva=0x%llX|vtable=0x%llX\n",
        argv[2], argv[3], static_cast<unsigned long long>(module.base),
        static_cast<unsigned long long>(module.size),
        static_cast<unsigned long long>(context.material_result),
        static_cast<unsigned long long>(
            context.material_result ? context.material_result - module.base : 0),
        static_cast<unsigned long long>(table));
    if (thread)
        CloseHandle(thread);
    if (wait == WAIT_OBJECT_0)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return valid ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--probe-move-helper") == 0) {
        ModuleInfo module{};
        module.path = argv[2];
        std::uint64_t object_rva = 0;
        std::uint64_t pointer_cell_rva = 0;
        const bool resolved = ResolveMoveHelperRvas(
            module, object_rva, pointer_cell_rva);
        std::printf("move_helper_object_rva=0x%llX "
                    "move_helper_pointer_cell_rva=0x%llX resolved=%d\n",
                    static_cast<unsigned long long>(object_rva),
                    static_cast<unsigned long long>(pointer_cell_rva),
                    resolved ? 1 : 0);
        return resolved ? 0 : 1;
    }
    if (argc == 4)
        return ResolveSingleInterface(argc, argv);
    if (argc != 2)
        return 2;
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const auto material = FindModule(pid, "materialsystem.dll");
    const auto input = FindModule(pid, "inputsystem.dll");
    const auto engine = FindModule(pid, "engine.dll");
    const auto client = FindModule(pid, "client.dll");
    const auto vstdlib = FindModule(pid, "vstdlib.dll");
    const auto lua_shared = FindModule(pid, "lua_shared.dll");
    const auto vgui2 = FindModule(pid, "vgui2.dll");
    const auto vgui_surface = FindModule(pid, "vguimatsurface.dll");
    const auto studio_render = FindModule(pid, "studiorender.dll");
    const auto material_rva = ExportRva(material, "CreateInterface");
    const auto input_rva = ExportRva(input, "CreateInterface");
    const auto engine_rva = ExportRva(engine, "CreateInterface");
    const auto client_rva = ExportRva(client, "CreateInterface");
    const auto vstdlib_rva = ExportRva(vstdlib, "CreateInterface");
    const auto lua_shared_rva = ExportRva(lua_shared, "CreateInterface");
    const auto vgui2_rva = ExportRva(vgui2, "CreateInterface");
    if (!material.base || !input.base || !engine.base || !client.base ||
        !vstdlib.base || !lua_shared.base || !vgui2.base ||
        !vgui_surface.base || !studio_render.base ||
        !material_rva || !input_rva || !engine_rva || !client_rva ||
        !vstdlib_rva || !lua_shared_rva || !vgui2_rva) {
        std::fprintf(stderr, "module or export resolution failed\n");
        return 1;
    }
    static constexpr auto view_render_pattern = std::to_array<int>({
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x48, 0x89, 0x05, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x48, 0x89, 0x05, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x48, 0x89, 0x05, -1, -1, -1, -1,
        0x48, 0x83, 0xC4, 0x28, 0xC3});
    static constexpr auto hostname_pattern = std::to_array<int>({
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xD1,
        0x48, 0x8B, 0x49, 0x78, 0x48, 0x8B, 0x01,
        0xFF, 0x90, 0x90, 0x01, 0x00, 0x00,
        0x48, 0x8B, 0x0D, -1, -1, -1, -1,
        0x48, 0x8D, 0x15, -1, -1, -1, -1,
        0x45, 0x33, 0xC0, 0x48, 0x8B, 0x01,
        0xFF, 0x90, 0xE8, 0x00, 0x00, 0x00,
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xC4, 0x28, 0xC3});
    static constexpr auto client_mode_pattern = std::to_array<int>({
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x30,
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
        0x8B, 0x0D, -1, -1, -1, -1,
        0xBA, 0x04, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x0C, 0xC8, 0x8B, 0x04, 0x0A,
        0x39, 0x05, -1, -1, -1, -1, 0x7F, 0x53,
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3,
        0x0F, 0x10, 0x44, 0x24, 0x20,
        0xBA, 0xFE, 0x08, 0x00, 0x00,
        0x48, 0x89, 0x3D, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0x0F, 0x11, 0x05, -1, -1, -1, -1,
        0xFF, 0x15, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x0D, -1, -1, -1, -1,
        0xE8, -1, -1, -1, -1,
        0x48, 0x8B, 0xC7, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3});
    static constexpr auto engine_boolean_pattern = std::to_array<int>({
        0x40, 0xB7, 0x01,
        0x48, 0x8B, 0x01, 0xFF, 0x50, 0x30, 0x84, 0xC0,
        0x74});
    static constexpr auto client_state_pattern = std::to_array<int>({
        0x8B, 0x05, -1, -1, -1, -1,
        0x48, 0x8D, 0x4D, 0x98,
        0x8B, 0x3D, -1, -1, -1, -1,
        0xFF, 0xC0, 0x03, 0xF8,
        0xC6, 0x44, 0x24, 0x38, 0x01,
        0x48, 0x8D, 0x05, -1, -1, -1, -1,
        0x4C, 0x89, 0x74, 0x24, 0x40,
        0xBE, 0xFF, 0xFF, 0xFF, 0xFF,
        0x48, 0x89, 0x44, 0x24, 0x30,
        0x44, 0x88, 0x74, 0x24, 0x68,
        0x4C, 0x89, 0x74, 0x24, 0x60,
        0x89, 0x74, 0x24, 0x6C,
        0x4C, 0x89, 0x74, 0x24, 0x70,
        0xE8, -1, -1, -1, -1,
        0x45, 0x33, 0xC9});
    const auto globals_rva = ResolveEngineGlobalsRva(engine);
    const auto view_render_rva = ResolveUniqueRipRva(
        client, view_render_pattern, 14, 18);
    const auto hostname_rva = ResolveUniqueRipRva(
        client, hostname_pattern, 30, 34);
    const auto client_mode_rva = ResolveUniqueRipRva(
        client, client_mode_pattern, 44, 48);
    const auto engine_code_rva = ResolveUniquePatternRva(
        engine, engine_boolean_pattern, 0);
    const auto client_state_field_rva = ResolveUniqueRipRva(
        engine, client_state_pattern, 2, 6);
    const auto client_state_rva = client_state_field_rva >= 0x580
                                      ? client_state_field_rva - 0x580
                                      : 0;
    const auto prediction_seed_rva = ResolvePredictionSeedRva(client);
    std::uint64_t move_helper_object_rva = 0;
    std::uint64_t move_helper_pointer_cell_rva = 0;
    const bool move_helper_rvas_ok = ResolveMoveHelperRvas(
        client, move_helper_object_rva, move_helper_pointer_cell_rva);
    if (!globals_rva || !view_render_rva || !hostname_rva ||
        !client_mode_rva || !engine_code_rva || !client_state_rva ||
        !prediction_seed_rva || !move_helper_rvas_ok) {
        std::fprintf(stderr,
                     "dynamic object resolution failed globals=0x%llX "
                     "view=0x%llX hostname=0x%llX client_mode=0x%llX "
                     "engine_boolean=0x%llX client_state=0x%llX "
                     "seed=0x%llX move_helper_object=0x%llX "
                     "move_helper_cell=0x%llX\n",
                     static_cast<unsigned long long>(globals_rva),
                     static_cast<unsigned long long>(view_render_rva),
                     static_cast<unsigned long long>(hostname_rva),
                     static_cast<unsigned long long>(client_mode_rva),
                     static_cast<unsigned long long>(engine_code_rva),
                     static_cast<unsigned long long>(client_state_rva),
                     static_cast<unsigned long long>(prediction_seed_rva),
                     static_cast<unsigned long long>(
                         move_helper_object_rva),
                     static_cast<unsigned long long>(
                         move_helper_pointer_cell_rva));
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
                                     PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                     PROCESS_VM_WRITE |
                                     kProcessSuspendResume,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed=%lu\n", GetLastError());
        return 1;
    }

    static constexpr std::array<std::uint8_t, 77> stub{
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB,
        0x48, 0x8B, 0x03, 0x48, 0x8B, 0x4B, 0x20, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x40, 0x48,
        0x8B, 0x43, 0x08, 0x48, 0x8B, 0x4B, 0x28, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x48, 0x48,
        0x8B, 0x43, 0x10, 0x48, 0x8B, 0x4B, 0x30, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x50, 0x48,
        0x8B, 0x43, 0x18, 0x48, 0x8B, 0x4B, 0x38, 0x31,
        0xD2, 0xFF, 0xD0, 0x48, 0x89, 0x43, 0x58, 0x48,
        0x83, 0xC4, 0x20, 0x5B, 0xC3,
    };
    const SIZE_T context_size = (sizeof(CallContext) + 15u) & ~SIZE_T(15u);
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, context_size + stub.size(),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote) {
        std::fprintf(stderr, "VirtualAllocEx failed=%lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    CallContext context{};
    context.material_factory = material.base + material_rva;
    context.input_factory = input.base + input_rva;
    context.engine_factory = engine.base + engine_rva;
    context.client_factory = client.base + client_rva;
    context.material_name = reinterpret_cast<std::uint64_t>(remote) +
                            offsetof(CallContext, names);
    std::strcpy(context.names, "VMaterialSystem080");
    const auto input_offset = std::strlen(context.names) + 1;
    std::strcpy(context.names + input_offset, "InputSystemVersion001");
    context.input_name = context.material_name + input_offset;
    const auto engine_offset = input_offset +
                               std::strlen(context.names + input_offset) + 1;
    std::strcpy(context.names + engine_offset, "VEngineClient015");
    context.engine_name = context.material_name + engine_offset;
    const auto client_offset = engine_offset +
                               std::strlen(context.names + engine_offset) + 1;
    std::strcpy(context.names + client_offset, "VClientEntityList003");
    context.client_name = context.material_name + client_offset;
    const auto cvar_offset =
        client_offset + std::strlen(context.names + client_offset) + 1;
    std::strcpy(context.names + cvar_offset, "VEngineCvar007");
    SIZE_T done = 0;
    if (!WriteProcessMemory(process, remote, &context, sizeof(context), &done) ||
        done != sizeof(context) ||
        !WriteProcessMemory(process, remote + context_size, stub.data(),
                            stub.size(), &done) ||
        done != stub.size()) {
        std::fprintf(stderr,
                     "remote context/stub write failed=%lu done=%zu expected=%zu\n",
                     GetLastError(), static_cast<std::size_t>(done),
                     stub.size());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    FlushInstructionCache(process, remote + context_size, stub.size());
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + context_size), remote,
        0, nullptr);
    const DWORD wait = thread ? WaitForSingleObject(thread, 30000) : WAIT_FAILED;
    done = 0;
    const bool read =
        ReadProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context);
    const std::uint64_t material_result = context.material_result;
    const std::uint64_t input_result = context.input_result;
    const std::uint64_t engine_result = context.engine_result;
    const std::uint64_t entity_result = context.client_result;
    context.client_factory = vstdlib.base + vstdlib_rva;
    context.client_name = context.material_name + cvar_offset;
    done = 0;
    const bool rewrote =
        read && wait == WAIT_OBJECT_0 &&
        WriteProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context);
    HANDLE cvar_thread = rewrote
                             ? CreateRemoteThread(
                                   process, nullptr, 0,
                                   reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                       remote + context_size),
                                   remote, 0, nullptr)
                             : nullptr;
    const DWORD cvar_wait =
        cvar_thread ? WaitForSingleObject(cvar_thread, 30000) : WAIT_FAILED;
    done = 0;
    const bool cvar_read =
        cvar_wait == WAIT_OBJECT_0 &&
        ReadProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context);
    const std::uint64_t cvar_result = context.client_result;
    context = {};
    context.material_factory = client.base + client_rva;
    context.input_factory = engine.base + engine_rva;
    context.engine_factory = lua_shared.base + lua_shared_rva;
    context.client_factory = vgui2.base + vgui2_rva;
    context.material_name = reinterpret_cast<std::uint64_t>(remote) +
                            offsetof(CallContext, names);
    std::strcpy(context.names, "VClient017");
    const auto model_offset = std::strlen(context.names) + 1;
    std::strcpy(context.names + model_offset, "VEngineModel016");
    context.input_name = context.material_name + model_offset;
    const auto lua_offset =
        model_offset + std::strlen(context.names + model_offset) + 1;
    std::strcpy(context.names + lua_offset, "LUASHARED003");
    context.engine_name = context.material_name + lua_offset;
    const auto panel_offset =
        lua_offset + std::strlen(context.names + lua_offset) + 1;
    std::strcpy(context.names + panel_offset, "VGUI_Panel009");
    context.client_name = context.material_name + panel_offset;
    done = 0;
    const bool interface_rewrote =
        cvar_read &&
        WriteProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context);
    HANDLE interface_thread =
        interface_rewrote
            ? CreateRemoteThread(
                  process, nullptr, 0,
                  reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + context_size),
                  remote, 0, nullptr)
            : nullptr;
    const DWORD interface_wait =
        interface_thread ? WaitForSingleObject(interface_thread, 30000)
                         : WAIT_FAILED;
    done = 0;
    const bool interface_read =
        interface_wait == WAIT_OBJECT_0 &&
        ReadProcessMemory(process, remote, &context, sizeof(context), &done) !=
            FALSE &&
        done == sizeof(context);
    const std::uint64_t client_result = context.material_result;
    const std::uint64_t model_result = context.input_result;
    const std::uint64_t lua_shared_result = context.engine_result;
    const std::uint64_t vgui_panel_result = context.client_result;
    std::uint64_t model_info_result = 0;
    std::uint64_t engine_trace_result = 0;
    std::uint64_t game_event_result = 0;
    std::uint64_t engine_vgui_result = 0;
    std::uint64_t render_view_result = 0;
    std::uint64_t game_movement_result = 0;
    std::uint64_t prediction_result = 0;
    std::uint64_t vgui_surface_result = 0;
    std::uint64_t studio_render_result = 0;
    const bool model_info_ok = ResolveInterfaceObject(
        process, engine, "VModelInfoClient006", model_info_result);
    const bool engine_trace_ok = ResolveInterfaceObject(
        process, engine, "EngineTraceClient003", engine_trace_result);
    const bool game_event_ok = ResolveInterfaceObject(
        process, engine, "GAMEEVENTSMANAGER002", game_event_result);
    const bool engine_vgui_ok = ResolveInterfaceObject(
        process, engine, "VEngineVGui001", engine_vgui_result);
    const bool render_view_ok = ResolveInterfaceObject(
        process, engine, "VEngineRenderView014", render_view_result);
    const bool game_movement_ok = ResolveInterfaceObject(
        process, client, "GameMovement001", game_movement_result);
    const bool prediction_ok = ResolveInterfaceObject(
        process, client, "VClientPrediction001", prediction_result);
    const bool vgui_surface_ok = ResolveInterfaceObject(
        process, vgui_surface, "VGUI_Surface030", vgui_surface_result);
    const bool studio_render_ok = ResolveInterfaceObject(
        process, studio_render, "VStudioRender025", studio_render_result);
    const std::uint64_t view_render_result = client.base + view_render_rva;
    const std::uint64_t client_mode_result = client.base + client_mode_rva;
    const std::uint64_t client_state_result = engine.base + client_state_rva;
    const std::uint64_t prediction_seed_result =
        client.base + prediction_seed_rva;
    const std::uint64_t server_hostname_result = client.base + hostname_rva;
    std::uint64_t native_input_result = 0;
    const bool native_input_ok = ResolveNativeInput(
        process, client, client_result, native_input_result);
    std::uint64_t move_helper_result = 0;
    const bool move_helper_ok = ResolveMoveHelper(
        process, client, move_helper_object_rva,
        move_helper_pointer_cell_rva, move_helper_result);
    std::uint64_t material_vtable = 0;
    std::uint64_t input_vtable = 0;
    std::uint64_t engine_vtable = 0;
    std::uint64_t client_vtable = 0;
    std::uint64_t client_slot_three = 0;
    std::uint64_t cvar_vtable = 0;
    std::uint64_t cvar_slot_zero = 0;
    std::uint64_t client_interface_vtable = 0;
    std::uint64_t model_vtable = 0;
    std::uint64_t lua_shared_vtable = 0;
    std::uint64_t vgui_panel_vtable = 0;
    std::uint64_t view_render_vtable = 0;
    std::uint64_t client_mode_vtable = 0;
    std::uint64_t client_mode_create_move = 0;
    const std::uint64_t engine_code_target = engine.base + engine_code_rva;
    MEMORY_BASIC_INFORMATION engine_code_memory{};
    const SIZE_T engine_code_query = VirtualQueryEx(
        process, reinterpret_cast<const void*>(engine_code_target),
        &engine_code_memory, sizeof(engine_code_memory));
    const std::uint64_t engine_code_region =
        reinterpret_cast<std::uint64_t>(engine_code_memory.BaseAddress);
    const bool engine_code_owned =
        engine_code_query == sizeof(engine_code_memory) &&
        engine_code_memory.State == MEM_COMMIT &&
        engine_code_memory.Type == MEM_IMAGE &&
        reinterpret_cast<std::uint64_t>(engine_code_memory.AllocationBase) ==
            engine.base &&
        engine_code_target >= engine_code_region &&
        engine_code_target - engine_code_region <
            engine_code_memory.RegionSize;
    const std::uint64_t globals_target = engine.base + globals_rva;
    std::array<std::uint8_t, kSendPacketInitializer.size()>
        engine_code_probe{};
    float absolute_frame_time = 0.0f;
    double interval_per_tick = 0.0;
    std::int32_t framecount = -1;
    const bool globals_valid =
        IsCommittedReadable(process, globals_target, 0x3C) &&
        ReadValue(process, globals_target + 0x08, absolute_frame_time) &&
        ReadValue(process, globals_target + 0x28, interval_per_tick) &&
        ReadValue(process, globals_target + 0x04, framecount) &&
        std::isfinite(absolute_frame_time) && absolute_frame_time >= 0.0f &&
        absolute_frame_time < 10.0f && std::isfinite(interval_per_tick) &&
        interval_per_tick > 0.0001 && interval_per_tick < 1.0 &&
        framecount >= 0;
    const bool engine_code_valid =
        engine_code_rva + kSendPacketInitializer.size() <= engine.size &&
        engine_code_rva >= kSendPacketCaveDistance &&
        engine_code_target >= engine.base && engine_code_owned &&
        IsCommittedExecutable(process, engine_code_target) &&
        IsCommittedReadable(process, engine_code_target,
                            engine_code_probe.size()) &&
        ReadBytes(process, engine_code_target, engine_code_probe.data(),
                  engine_code_probe.size()) &&
        engine_code_probe == kSendPacketInitializer &&
        IsCommittedReadable(process, kEngineCodeSlot, sizeof(std::uint64_t));
    const bool extended_valid =
        model_info_ok && engine_trace_ok && game_event_ok && engine_vgui_ok &&
        render_view_ok && game_movement_ok && prediction_ok &&
        vgui_surface_ok && studio_render_ok && native_input_ok &&
        move_helper_ok &&
        ReadQword(process, view_render_result, view_render_vtable) &&
        view_render_vtable >= client.base &&
        view_render_vtable < client.base + client.size &&
        ReadQword(process, client_mode_result, client_mode_vtable) &&
        client_mode_vtable >= client.base &&
        client_mode_vtable < client.base + client.size &&
        ReadQword(process,
                  client_mode_vtable + 21 * sizeof(std::uint64_t),
                  client_mode_create_move) &&
        client_mode_create_move >= client.base &&
        client_mode_create_move < client.base + client.size &&
        IsCommittedExecutable(process, client_mode_create_move) &&
        IsCommittedReadable(process, client_state_result, 0x600) &&
        IsCommittedReadable(process, native_input_result, 0xE0) &&
        IsCommittedWritableNonExecutable(process, prediction_seed_result,
                                         sizeof(std::uint32_t)) &&
        IsCommittedReadable(process, server_hostname_result, 1);
    const bool prevalidated =
        read && wait == WAIT_OBJECT_0 && cvar_read &&
        cvar_wait == WAIT_OBJECT_0 && interface_read &&
        interface_wait == WAIT_OBJECT_0 && material_result != 0 &&
        input_result != 0 && engine_result != 0 && client_result != 0 &&
        model_result != 0 && lua_shared_result != 0 &&
        vgui_panel_result != 0 && entity_result != 0 && cvar_result != 0 &&
        ReadQword(process, material_result, material_vtable) &&
        ReadQword(process, input_result, input_vtable) &&
        ReadQword(process, engine_result, engine_vtable) &&
        ReadQword(process, entity_result, client_vtable) &&
        ReadQword(process, client_vtable + 3 * sizeof(void*),
                  client_slot_three) &&
        ReadQword(process, cvar_result, cvar_vtable) &&
        ReadQword(process, cvar_vtable, cvar_slot_zero) &&
        ReadQword(process, client_result, client_interface_vtable) &&
        ReadQword(process, model_result, model_vtable) &&
        ReadQword(process, lua_shared_result, lua_shared_vtable) &&
        ReadQword(process, vgui_panel_result, vgui_panel_vtable) &&
        material_vtable >= material.base &&
        material_vtable < material.base + material.size &&
        input_vtable >= input.base && input_vtable < input.base + input.size &&
        engine_vtable >= engine.base &&
        engine_vtable < engine.base + engine.size &&
        client_vtable >= client.base &&
        client_vtable < client.base + client.size &&
        client_slot_three >= client.base &&
        client_slot_three < client.base + client.size &&
        IsCommittedExecutable(process, client_slot_three) &&
        cvar_vtable >= vstdlib.base &&
        cvar_vtable < vstdlib.base + vstdlib.size &&
        cvar_slot_zero >= vstdlib.base &&
        cvar_slot_zero < vstdlib.base + vstdlib.size &&
        IsCommittedExecutable(process, cvar_slot_zero) &&
        client_interface_vtable >= client.base &&
        client_interface_vtable < client.base + client.size &&
        model_vtable >= engine.base &&
        model_vtable < engine.base + engine.size &&
        lua_shared_vtable >= lua_shared.base &&
        lua_shared_vtable < lua_shared.base + lua_shared.size &&
        vgui_panel_vtable >= vgui2.base &&
        vgui_panel_vtable < vgui2.base + vgui2.size && globals_valid &&
        extended_valid && engine_code_valid;
    const bool payload_slots_written =
        prevalidated &&
                       WriteQword(process, kMaterialSlot, material_result) &&
                       WriteQword(process, kInputSlot, input_result) &&
                       WriteQword(process, kClientSlot, client_result) &&
                       WriteQword(process, kModelSlot, model_result) &&
                       WriteQword(process, kLuaSharedSlot,
                                  lua_shared_result) &&
                       WriteQword(process, kVguiPanelSlot,
                                  vgui_panel_result) &&
                       WriteQword(process, kClientEntityListSlot,
                                  entity_result) &&
                       WriteQword(process, kCvarSlot, cvar_result) &&
                       WriteQword(process, kEngineSlot, engine_result) &&
                       WriteQword(process, kClientStateSlot,
                                  client_state_result) &&
                       WriteQword(process, kPredictionEarlySlot,
                                  prediction_result) &&
                       WriteQword(process, kRenderViewSlot,
                                  render_view_result) &&
                       WriteQword(process, kStudioRenderSlot,
                                  studio_render_result) &&
                       WriteQword(process, kViewRenderSlot,
                                  view_render_result) &&
                       WriteQword(process, kModelDuplicateSlot,
                                  model_result) &&
                       WriteQword(process, kNativeInputSlot,
                                  native_input_result) &&
                       WriteQword(process, kPredictionSeedSlot,
                                  prediction_seed_result) &&
                       WriteQword(process, kEngineVguiSlot,
                                  engine_vgui_result) &&
                       WriteQword(process, kGameEventSlot,
                                  game_event_result) &&
                       WriteQword(process, kServerHostnameSlot,
                                  server_hostname_result) &&
                       WriteQword(process, kMoveHelperSlot,
                                  move_helper_result) &&
                       WriteQword(process, kGameMovementSlot,
                                  game_movement_result) &&
                       WriteQword(process, kClientModeSlot,
                                  client_mode_result) &&
                       WriteQword(process, kModelInfoSlot,
                                  model_info_result) &&
                       WriteQword(process, kVguiSurfaceSlot,
                                  vgui_surface_result) &&
                       WriteQword(process, kEngineTraceSlot,
                                  engine_trace_result) &&
                       WriteQword(process, kGlobalsSlot, globals_target);
    std::uint64_t send_packet_shadow = 0;
    std::uint64_t send_packet_cave = 0;
    std::uint64_t send_packet_original_slot = 0;
    DWORD send_packet_engine_protection = 0;
    int send_packet_failure_stage = 0;
    const bool send_packet_bridge_installed =
        payload_slots_written &&
        InstallSendPacketBridge(
            process, engine, engine_code_rva, kEngineCodeSlot,
            send_packet_shadow, send_packet_cave,
            send_packet_engine_protection, send_packet_original_slot,
            send_packet_failure_stage);
    const bool valid = send_packet_bridge_installed;
    std::uint64_t material_verified = 0;
    std::uint64_t input_verified = 0;
    std::uint64_t client_verified = 0;
    std::uint64_t cvar_verified = 0;
    std::uint64_t engine_verified = 0;
    std::uint64_t client_interface_verified = 0;
    std::uint64_t model_verified = 0;
    std::uint64_t lua_shared_verified = 0;
    std::uint64_t vgui_panel_verified = 0;
    std::uint64_t globals_verified = 0;
    std::uint64_t engine_code_verified = 0;
    std::uint64_t client_state_verified = 0;
    std::uint64_t prediction_early_verified = 0;
    std::uint64_t render_view_verified = 0;
    std::uint64_t studio_render_verified = 0;
    std::uint64_t view_render_verified = 0;
    std::uint64_t model_duplicate_verified = 0;
    std::uint64_t native_input_verified = 0;
    std::uint64_t prediction_seed_verified = 0;
    std::uint64_t engine_vgui_verified = 0;
    std::uint64_t game_event_verified = 0;
    std::uint64_t server_hostname_verified = 0;
    std::uint64_t move_helper_verified = 0;
    std::uint64_t game_movement_verified = 0;
    std::uint64_t client_mode_verified = 0;
    std::uint64_t model_info_verified = 0;
    std::uint64_t vgui_surface_verified = 0;
    std::uint64_t engine_trace_verified = 0;
    const bool verified =
        valid && ReadQword(process, kMaterialSlot, material_verified) &&
        ReadQword(process, kInputSlot, input_verified) &&
        ReadQword(process, kClientEntityListSlot, client_verified) &&
        ReadQword(process, kCvarSlot, cvar_verified) &&
        ReadQword(process, kEngineSlot, engine_verified) &&
        ReadQword(process, kClientSlot, client_interface_verified) &&
        ReadQword(process, kModelSlot, model_verified) &&
        ReadQword(process, kLuaSharedSlot, lua_shared_verified) &&
        ReadQword(process, kVguiPanelSlot, vgui_panel_verified) &&
        ReadQword(process, kGlobalsSlot, globals_verified) &&
        ReadQword(process, kEngineCodeSlot, engine_code_verified) &&
        ReadQword(process, kClientStateSlot, client_state_verified) &&
        ReadQword(process, kPredictionEarlySlot,
                  prediction_early_verified) &&
        ReadQword(process, kRenderViewSlot, render_view_verified) &&
        ReadQword(process, kStudioRenderSlot, studio_render_verified) &&
        ReadQword(process, kViewRenderSlot, view_render_verified) &&
        ReadQword(process, kModelDuplicateSlot,
                  model_duplicate_verified) &&
        ReadQword(process, kNativeInputSlot, native_input_verified) &&
        ReadQword(process, kPredictionSeedSlot,
                  prediction_seed_verified) &&
        ReadQword(process, kEngineVguiSlot, engine_vgui_verified) &&
        ReadQword(process, kGameEventSlot, game_event_verified) &&
        ReadQword(process, kServerHostnameSlot,
                  server_hostname_verified) &&
        ReadQword(process, kMoveHelperSlot, move_helper_verified) &&
        ReadQword(process, kGameMovementSlot,
                  game_movement_verified) &&
        ReadQword(process, kClientModeSlot, client_mode_verified) &&
        ReadQword(process, kModelInfoSlot, model_info_verified) &&
        ReadQword(process, kVguiSurfaceSlot, vgui_surface_verified) &&
        ReadQword(process, kEngineTraceSlot, engine_trace_verified) &&
        material_verified == material_result &&
        input_verified == input_result &&
        client_verified == entity_result && cvar_verified == cvar_result &&
        engine_verified == engine_result &&
        client_interface_verified == client_result &&
        model_verified == model_result &&
        lua_shared_verified == lua_shared_result &&
        vgui_panel_verified == vgui_panel_result &&
        globals_verified == globals_target &&
        engine_code_verified == send_packet_shadow &&
        client_state_verified == client_state_result &&
        prediction_early_verified == prediction_result &&
        render_view_verified == render_view_result &&
        studio_render_verified == studio_render_result &&
        view_render_verified == view_render_result &&
        model_duplicate_verified == model_result &&
        native_input_verified == native_input_result &&
        prediction_seed_verified == prediction_seed_result &&
        engine_vgui_verified == engine_vgui_result &&
        game_event_verified == game_event_result &&
        server_hostname_verified == server_hostname_result &&
        move_helper_verified == move_helper_result &&
        game_movement_verified == game_movement_result &&
        client_mode_verified == client_mode_result &&
        model_info_verified == model_info_result &&
        vgui_surface_verified == vgui_surface_result &&
        engine_trace_verified == engine_trace_result &&
        VerifySendPacketBridge(process, engine, engine_code_rva,
                               kEngineCodeSlot, send_packet_shadow);
    const bool bridge_cleanup_ok =
        verified || !send_packet_bridge_installed ||
        RemoveSendPacketBridge(process, engine, engine_code_rva,
                               kEngineCodeSlot, send_packet_shadow,
                               send_packet_original_slot);
    std::printf("pid=%lu material_base=0x%llX factory_rva=0x%llX object=0x%llX vtable=0x%llX input_base=0x%llX factory_rva=0x%llX object=0x%llX vtable=0x%llX engine_base=0x%llX factory_rva=0x%llX object=0x%llX vtable=0x%llX client_base=0x%llX factory_rva=0x%llX entity_list=0x%llX vtable=0x%llX slot3=0x%llX vstdlib_base=0x%llX cvar=0x%llX cvar_vtable=0x%llX globals_rva=0x%llX globals=0x%llX absolute_frame_time=%.9f interval=%.9f framecount=%d code_target=0x%llX code_signature=%02X%02X%02X code_valid=%d shadow=0x%llX cave=0x%llX protection=0x%lX bridge_stage=%d bridge_error=0x%lX cleanup=%d wait=%lu cvar_wait=%lu verified=%d\n",
                pid, static_cast<unsigned long long>(material.base),
                static_cast<unsigned long long>(material_rva),
                static_cast<unsigned long long>(material_result),
                static_cast<unsigned long long>(material_vtable),
                static_cast<unsigned long long>(input.base),
                static_cast<unsigned long long>(input_rva),
                static_cast<unsigned long long>(input_result),
                static_cast<unsigned long long>(input_vtable),
                static_cast<unsigned long long>(engine.base),
                static_cast<unsigned long long>(engine_rva),
                static_cast<unsigned long long>(engine_result),
                static_cast<unsigned long long>(engine_vtable),
                static_cast<unsigned long long>(client.base),
                static_cast<unsigned long long>(client_rva),
                static_cast<unsigned long long>(entity_result),
                static_cast<unsigned long long>(client_vtable),
                static_cast<unsigned long long>(client_slot_three),
                static_cast<unsigned long long>(vstdlib.base),
                static_cast<unsigned long long>(cvar_result),
                static_cast<unsigned long long>(cvar_vtable),
                static_cast<unsigned long long>(globals_rva),
                static_cast<unsigned long long>(globals_target),
                static_cast<double>(absolute_frame_time), interval_per_tick,
                framecount,
                static_cast<unsigned long long>(engine_code_target),
                static_cast<unsigned int>(engine_code_probe[0]),
                static_cast<unsigned int>(engine_code_probe[1]),
                static_cast<unsigned int>(engine_code_probe[2]),
                engine_code_valid ? 1 : 0,
                static_cast<unsigned long long>(send_packet_shadow),
                static_cast<unsigned long long>(send_packet_cave),
                static_cast<unsigned long>(send_packet_engine_protection),
                send_packet_failure_stage,
                static_cast<unsigned long>(GetLastError()),
                bridge_cleanup_ok ? 1 : 0, wait, cvar_wait,
                verified ? 1 : 0);
    std::printf(
        "extended model_info=0x%llX trace=0x%llX events=0x%llX "
        "engine_vgui=0x%llX render_view=0x%llX studio=0x%llX "
        "view=0x%llX native_input=0x%llX seed=0x%llX "
        "hostname=0x%llX move_helper=0x%llX movement=0x%llX "
        "prediction=0x%llX "
        "client_mode=0x%llX client_state=0x%llX "
        "surface=0x%llX valid=%d\n",
        static_cast<unsigned long long>(model_info_result),
        static_cast<unsigned long long>(engine_trace_result),
        static_cast<unsigned long long>(game_event_result),
        static_cast<unsigned long long>(engine_vgui_result),
        static_cast<unsigned long long>(render_view_result),
        static_cast<unsigned long long>(studio_render_result),
        static_cast<unsigned long long>(view_render_result),
        static_cast<unsigned long long>(native_input_result),
        static_cast<unsigned long long>(prediction_seed_result),
        static_cast<unsigned long long>(server_hostname_result),
        static_cast<unsigned long long>(move_helper_result),
        static_cast<unsigned long long>(game_movement_result),
        static_cast<unsigned long long>(prediction_result),
        static_cast<unsigned long long>(client_mode_result),
        static_cast<unsigned long long>(client_state_result),
        static_cast<unsigned long long>(vgui_surface_result),
        extended_valid ? 1 : 0);
    if (thread)
        CloseHandle(thread);
    if (cvar_thread)
        CloseHandle(cvar_thread);
    if (interface_thread)
        CloseHandle(interface_thread);
    if (wait == WAIT_OBJECT_0 && cvar_wait == WAIT_OBJECT_0 &&
        interface_wait == WAIT_OBJECT_0)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return verified ? 0 : 1;
}
