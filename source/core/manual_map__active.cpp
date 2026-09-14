#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "kirkware_import_rebuilder.hpp"
#include "kirkware_bone_access_compat.hpp"
#include "kirkware_scoped_early_auth_hook.hpp"
#include "kirkware_network_guard.hpp"

namespace {

constexpr std::uint64_t kImageBase = 0x000001E5DCC00000ull;
constexpr std::size_t kImageSize = 0x1162000;
constexpr std::size_t kBootstrapImageSize = 0x1163000;
constexpr std::uint32_t kBootstrapEntryRva = 0x1162000;
constexpr std::uint32_t kTlsDataRva = 0x696D40;
constexpr std::size_t kTlsDataSize = 0x14C;
constexpr std::uint32_t kTlsIndexRva = 0x7E4790;
constexpr std::uint32_t kExceptionRva = 0x1126890;
constexpr std::uint32_t kExceptionCount = 0x4EE4;

struct Page {
    std::uint64_t original_base;
    std::string file;
    std::uint64_t remote_base;
};

struct RemoteCallContext {
    std::uint64_t function;
    std::uint64_t argument1;
    std::uint64_t argument2;
    std::uint64_t argument3;
    std::uint64_t result;
};

using NtMapViewOfSectionFn = LONG(NTAPI*)(HANDLE, HANDLE, void**, ULONG_PTR,
                                          SIZE_T, LARGE_INTEGER*, SIZE_T*,
                                          DWORD, ULONG, ULONG);
using NtUnmapViewOfSectionFn = LONG(NTAPI*)(HANDLE, void*);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG,
                                                  ULONG*);
using NtSuspendProcessFn = LONG(NTAPI*)(HANDLE);
using NtResumeProcessFn = LONG(NTAPI*)(HANDLE);

class ProcessSuspension {
  public:
    explicit ProcessSuspension(HANDLE process) {
        DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
                        &process_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        suspend_ = reinterpret_cast<NtSuspendProcessFn>(
            GetProcAddress(ntdll, "NtSuspendProcess"));
        resume_ = reinterpret_cast<NtResumeProcessFn>(
            GetProcAddress(ntdll, "NtResumeProcess"));
    }

    ~ProcessSuspension() {
        Resume();
        if (process_)
            CloseHandle(process_);
    }

    bool Suspend() {
        if (suspended_)
            return true;
        suspended_ = process_ && suspend_ && resume_ && suspend_(process_) >= 0;
        return suspended_;
    }

    bool Resume() {
        if (!suspended_)
            return true;
        if (!resume_ || resume_(process_) < 0)
            return false;
        suspended_ = false;
        return true;
    }

  private:
    HANDLE process_ = nullptr;
    NtSuspendProcessFn suspend_ = nullptr;
    NtResumeProcessFn resume_ = nullptr;
    bool suspended_ = false;
};

struct ProcessBasicInformation {
    LONG exit_status;
    void* peb_base;
    ULONG_PTR affinity_mask;
    LONG base_priority;
    ULONG_PTR process_id;
    ULONG_PTR inherited_process_id;
};

#if defined(KIRKWARE_UNLINK_ACTIVE_IMAGE)
struct RemoteListEntry {
    std::uint64_t flink;
    std::uint64_t blink;
};
#endif

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::uint64_t RemoteModuleBase(DWORD pid, const std::string& requested) {
    const auto wanted = Lower(requested);
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH) {
                Sleep(2);
                continue;
            }
            return 0;
        }
        MODULEENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::uint64_t result = 0;
        BOOL next = Module32First(snapshot, &entry);
        while (next) {
            if (Lower(entry.szModule) == wanted ||
                Lower(BaseName(entry.szExePath)) == wanted) {
                result = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
                break;
            }
            next = Module32Next(snapshot, &entry);
        }
        const DWORD enumeration_error = GetLastError();
        CloseHandle(snapshot);
        if (result)
            return result;
        if (!next && enumeration_error == ERROR_BAD_LENGTH) {
            Sleep(2);
            continue;
        }
        return 0;
    }
    return 0;
}

bool ValidateBootstrapImage(HANDLE process) {
    const std::uint64_t image_end = kImageBase + kBootstrapImageSize;
    std::uint64_t cursor = kImageBase;
    while (cursor < image_end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                           &region, sizeof(region)) != sizeof(region))
            return false;
        const auto region_base =
            reinterpret_cast<std::uint64_t>(region.BaseAddress);
        const auto allocation_base =
            reinterpret_cast<std::uint64_t>(region.AllocationBase);
        if (region.RegionSize == 0 || region_base > cursor ||
            allocation_base != kImageBase || region.State != MEM_COMMIT ||
            region.Type != MEM_IMAGE ||
            region_base > UINT64_MAX - region.RegionSize)
            return false;
        const std::uint64_t region_end = region_base + region.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = std::min(region_end, image_end);
    }

    IMAGE_DOS_HEADER dos{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(kImageBase),
                           &dos, sizeof(dos), &read) ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<std::uint32_t>(dos.e_lfanew) >
            0x1000u - sizeof(IMAGE_NT_HEADERS64))
        return false;

    IMAGE_NT_HEADERS64 nt{};
    read = 0;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(
                kImageBase + static_cast<std::uint32_t>(dos.e_lfanew)),
            &nt, sizeof(nt), &read) ||
        read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.NumberOfSections != 9 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.ImageBase != kImageBase ||
        nt.OptionalHeader.SizeOfImage != kBootstrapImageSize ||
        nt.OptionalHeader.AddressOfEntryPoint != kBootstrapEntryRva)
        return false;

    constexpr std::array<std::uint8_t, 6> entry_expected{
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    std::array<std::uint8_t, entry_expected.size()> entry{};
    read = 0;
    return ReadProcessMemory(
               process,
               reinterpret_cast<const void*>(kImageBase + kBootstrapEntryRva),
               entry.data(), entry.size(), &read) &&
           read == entry.size() && entry == entry_expected;
}

std::string RemoteModulePath(DWORD pid, const std::string& requested) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto wanted = Lower(requested);
    std::string result;
    if (Module32First(snapshot, &entry)) {
        do {
            if (Lower(entry.szModule) == wanted ||
                Lower(BaseName(entry.szExePath)) == wanted) {
                result = entry.szExePath;
                break;
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool IsGameModule(const std::string& module) {
    const auto name = Lower(BaseName(module));
    return name == "client.dll" || name == "engine.dll" ||
           name == "inputsystem.dll" || name == "lua_shared.dll" ||
           name == "materialsystem.dll" || name == "studiorender.dll" ||
           name == "vgui2.dll" || name == "vguimatsurface.dll" ||
           name == "vstdlib.dll";
}

std::uint64_t ResolveRemoteExport(DWORD pid, const char* requested_module,
                                  const char* symbol) {
    const auto remote_requested = RemoteModuleBase(pid, requested_module);
    if (!remote_requested)
        return 0;
    bool exact_remote_image = false;
    HMODULE requested = nullptr;
    if (IsGameModule(requested_module)) {
        const auto remote_path = RemoteModulePath(pid, requested_module);
        if (!remote_path.empty()) {
            requested = LoadLibraryExA(remote_path.c_str(), nullptr,
                                       DONT_RESOLVE_DLL_REFERENCES);
            exact_remote_image = requested != nullptr;
        }
    } else {
        requested = GetModuleHandleA(requested_module);
        if (!requested)
            requested = LoadLibraryExA(requested_module, nullptr,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!requested)
        return 0;
    FARPROC address = GetProcAddress(requested, symbol);
    if (!address)
        return 0;
    if (exact_remote_image) {
        return remote_requested +
               (reinterpret_cast<std::uint64_t>(address) -
                reinterpret_cast<std::uint64_t>(requested));
    }
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

std::vector<std::uint8_t> ReadBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), size);
    if (!input.good())
        return {};
    return result;
}

std::vector<std::string> Csv(const std::string& line) {
    std::vector<std::string> result;
    std::string item;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == ',' && !quoted) {
            result.push_back(item);
            item.clear();
        } else {
            item.push_back(c);
        }
    }
    result.push_back(item);
    return result;
}

bool Write(HANDLE process, std::uint64_t address, const void* data,
           std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, reinterpret_cast<void*>(address), data,
                              size, &written) != FALSE && written == size;
}

bool RemoteCall3(HANDLE process, std::uint64_t function,
                 std::uint64_t argument1, std::uint64_t argument2,
                 std::uint64_t argument3, std::uint64_t& result) {
    static constexpr std::uint8_t stub[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8B, 0x53, 0x10,
        0x4C, 0x8B, 0x43, 0x18, 0xFF, 0x13,
        0x48, 0x89, 0x43, 0x20, 0x48, 0x83, 0xC4, 0x20,
        0x5B, 0xC3};
    RemoteCallContext context{function, argument1, argument2, argument3, 0};
    const SIZE_T context_size = (sizeof(context) + 15u) & ~SIZE_T{15u};
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, context_size + sizeof(stub),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote ||
        !Write(process, reinterpret_cast<std::uint64_t>(remote),
               &context, sizeof(context)) ||
        !Write(process,
               reinterpret_cast<std::uint64_t>(remote + context_size),
               stub, sizeof(stub))) {
        if (remote)
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(process, remote + context_size, sizeof(stub));
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + context_size),
        remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    SIZE_T read = 0;
    const bool ok = wait == WAIT_OBJECT_0 &&
                    ReadProcessMemory(process, remote, &context,
                                      sizeof(context), &read) != FALSE &&
                    read == sizeof(context);
    CloseHandle(thread);
    if (wait == WAIT_OBJECT_0)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (!ok)
        return false;
    result = context.result;
    return true;
}

bool PatchRemoteLoaderEntry(HANDLE process) {
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQueryInformationProcess"));
    ProcessBasicInformation basic{};
    if (!query || query(process, 0, &basic, sizeof(basic), nullptr) < 0 ||
        !basic.peb_base)
        return false;

    std::uint64_t ldr = 0;
    SIZE_T read = 0;
    const auto peb = reinterpret_cast<std::uint64_t>(basic.peb_base);
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(peb + 0x18),
                           &ldr, sizeof(ldr), &read) ||
        read != sizeof(ldr) || !ldr)
        return false;

    const std::uint64_t head = ldr + 0x10;
    std::uint64_t current = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(head),
                           &current, sizeof(current), &read) ||
        read != sizeof(current))
        return false;

    for (unsigned index = 0; current && current != head && index < 512;
         ++index) {
        std::uint64_t next = 0;
        std::uint64_t dll_base = 0;
        if (!ReadProcessMemory(process,
                               reinterpret_cast<const void*>(current),
                               &next, sizeof(next), &read) ||
            read != sizeof(next) ||
            !ReadProcessMemory(process,
                               reinterpret_cast<const void*>(current + 0x30),
                               &dll_base, sizeof(dll_base), &read) ||
            read != sizeof(dll_base))
            return false;
        if (dll_base == kImageBase) {
            const std::uint64_t no_entry_point = 0;
            const std::uint32_t exact_image_size =
                static_cast<std::uint32_t>(kImageSize);
            return Write(process, current + 0x38, &no_entry_point,
                         sizeof(no_entry_point)) &&
                   Write(process, current + 0x40, &exact_image_size,
                         sizeof(exact_image_size));
        }
        current = next;
    }
    return false;
}

#if defined(KIRKWARE_UNLINK_ACTIVE_IMAGE)
std::uint64_t FindRemoteLoaderEntry(HANDLE process) {
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQueryInformationProcess"));
    ProcessBasicInformation basic{};
    if (!query || query(process, 0, &basic, sizeof(basic), nullptr) < 0 ||
        !basic.peb_base)
        return 0;
    std::uint64_t ldr = 0;
    SIZE_T read = 0;
    const auto peb = reinterpret_cast<std::uint64_t>(basic.peb_base);
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const void*>(peb + 0x18),
                           &ldr, sizeof(ldr), &read) ||
        read != sizeof(ldr) || !ldr)
        return 0;
    const std::uint64_t head = ldr + 0x10;
    std::uint64_t current = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(head),
                           &current, sizeof(current), &read) ||
        read != sizeof(current))
        return 0;
    for (unsigned index = 0; current && current != head && index < 512;
         ++index) {
        std::uint64_t next = 0;
        std::uint64_t dll_base = 0;
        if (!ReadProcessMemory(process,
                               reinterpret_cast<const void*>(current),
                               &next, sizeof(next), &read) ||
            read != sizeof(next) ||
            !ReadProcessMemory(process,
                               reinterpret_cast<const void*>(current + 0x30),
                               &dll_base, sizeof(dll_base), &read) ||
            read != sizeof(dll_base))
            return 0;
        if (dll_base == kImageBase)
            return current;
        current = next;
    }
    return 0;
}

bool UnlinkRemotePublicLoaderLists(HANDLE process,
                                   std::uint64_t loader_entry) {
    constexpr std::uint64_t offsets[] = {0, 0x10, 0x20};
    RemoteListEntry entries[3]{};
    SIZE_T read = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto address = loader_entry + offsets[index];
        if (!ReadProcessMemory(process,
                               reinterpret_cast<const void*>(address),
                               &entries[index], sizeof(entries[index]),
                               &read) ||
            read != sizeof(entries[index]) || !entries[index].flink ||
            !entries[index].blink)
            return false;
    }
    std::size_t unlinked = 0;
    for (; unlinked < 3; ++unlinked) {
        const auto index = unlinked;
        const auto address = loader_entry + offsets[index];
        const auto& entry = entries[index];
        const RemoteListEntry self{address, address};
        if (!Write(process, entry.blink, &entry.flink,
                   sizeof(entry.flink)) ||
            !Write(process, entry.flink + sizeof(std::uint64_t),
                   &entry.blink, sizeof(entry.blink)) ||
            !Write(process, address, &self, sizeof(self))) {
            Write(process, entry.blink, &address, sizeof(address));
            Write(process, entry.flink + sizeof(std::uint64_t), &address,
                  sizeof(address));
            Write(process, address, &entry, sizeof(entry));
            break;
        }
    }
    if (unlinked != 3) {
        while (unlinked > 0) {
            --unlinked;
            const auto address = loader_entry + offsets[unlinked];
            const auto& entry = entries[unlinked];
            Write(process, entry.blink, &address, sizeof(address));
            Write(process, entry.flink + sizeof(std::uint64_t), &address,
                  sizeof(address));
            Write(process, address, &entry, sizeof(entry));
        }
        return false;
    }
    for (std::size_t index = 0; index < 3; ++index) {
        const auto address = loader_entry + offsets[index];
        RemoteListEntry actual{};
        if (!ReadProcessMemory(process,
                               reinterpret_cast<const void*>(address),
                               &actual, sizeof(actual), &read) ||
            read != sizeof(actual) || actual.flink != address ||
            actual.blink != address)
            return false;
    }
    return true;
}
#endif

bool PatchAuxiliary(std::vector<std::uint8_t>& image, DWORD pid) {
    for (const auto& item : kirkware::auxiliary_patches) {
        std::uint64_t value = 0;
        if (item.kind == kirkware::AuxiliaryKind::export_name) {
            if (_stricmp(item.module, "kernel32.dll") == 0 &&
                std::strcmp(item.symbol, "GetTempPath2W") == 0) {
                value = ResolveRemoteExport(pid, "kernel32.dll",
                                            "GetTempPathW");
            } else {
                value = ResolveRemoteExport(pid, item.module, item.symbol);
            }
        } else {
            const auto remote_module = RemoteModuleBase(pid, item.module);
            if (remote_module)
                value = remote_module + item.module_rva;
        }
        if (!value || item.image_rva + sizeof(value) > image.size()) {
            std::fprintf(stderr,
                         "auxiliary resolution failed: rva=0x%llX module=%s\n",
                         static_cast<unsigned long long>(item.image_rva),
                         item.module);
            return false;
        }
        std::memcpy(image.data() + item.image_rva, &value, sizeof(value));
    }
    return true;
}

bool IsCommittedExecutableImageAddress(HANDLE process,
                                       std::uint64_t address) {
    if (!address ||
        (address >= kImageBase && address < kImageBase + kImageSize))
        return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                       &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        region.RegionSize == 0)
        return false;
    const auto region_base =
        reinterpret_cast<std::uint64_t>(region.BaseAddress);
    if (region_base > UINT64_MAX - region.RegionSize)
        return false;
    const auto region_end = region_base + region.RegionSize;
    if (address < region_base || address >= region_end)
        return false;
    const DWORD protection = region.Protect & 0xffu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool PatchFormalImports(
    std::vector<std::uint8_t>& image, HANDLE process, DWORD pid,
    bool preserve_snapshot_iat,
    const kirkware::NetworkGuard& network_guard,
    std::vector<kirkware::GuardedImportSlot>& guarded_slots,
    std::size_t& patched) {
    constexpr std::uint64_t descriptor_rva = 0x6F3114;
    patched = 0;
    for (std::size_t descriptor_index = 0; descriptor_index < 64;
         ++descriptor_index) {
        const auto descriptor_offset =
            descriptor_rva +
            descriptor_index * sizeof(kirkware::ImportDescriptor);
        if (!kirkware::in_image(image.size(), descriptor_offset,
                                     sizeof(kirkware::ImportDescriptor)))
            return false;
        kirkware::ImportDescriptor descriptor{};
        std::memcpy(&descriptor, image.data() + descriptor_offset,
                    sizeof(descriptor));
        if (!(descriptor.original_first_thunk | descriptor.timestamp |
              descriptor.forwarder_chain | descriptor.name |
              descriptor.first_thunk))
            return descriptor_index == 11 && patched == 324 &&
                   guarded_slots.size() ==
                       kirkware::kExpectedGuardedImportCount;
        if (!descriptor.original_first_thunk || !descriptor.name ||
            !descriptor.first_thunk ||
            !kirkware::in_image(image.size(), descriptor.name, 1))
            return false;
        const auto* module = reinterpret_cast<const char*>(
            image.data() + descriptor.name);
        for (std::size_t thunk_index = 0; thunk_index < 4096;
             ++thunk_index) {
            const auto lookup_offset =
                static_cast<std::uint64_t>(descriptor.original_first_thunk) +
                thunk_index * 8;
            const auto target_offset =
                static_cast<std::uint64_t>(descriptor.first_thunk) +
                thunk_index * 8;
            if (!kirkware::in_image(image.size(), lookup_offset, 8) ||
                !kirkware::in_image(image.size(), target_offset, 8))
                return false;
            std::uint64_t lookup = 0;
            std::memcpy(&lookup, image.data() + lookup_offset,
                        sizeof(lookup));
            if (!lookup) {
                const std::uint64_t terminator = 0;
                std::memcpy(image.data() + target_offset, &terminator,
                            sizeof(terminator));
                break;
            }
            const bool by_ordinal = (lookup & IMAGE_ORDINAL_FLAG64) != 0;
            const std::uint16_t ordinal =
                by_ordinal
                    ? static_cast<std::uint16_t>(IMAGE_ORDINAL64(lookup))
                    : 0;
            const char* symbol = nullptr;
            if (by_ordinal) {
                symbol = MAKEINTRESOURCEA(
                    static_cast<WORD>(ordinal));
            } else {
                if (!kirkware::in_image(image.size(), lookup + 2, 1))
                    return false;
                symbol = reinterpret_cast<const char*>(image.data() +
                                                        lookup + 2);
            }
            auto target = kirkware::guarded_import_target(
                network_guard, module, by_ordinal ? nullptr : symbol,
                by_ordinal, ordinal);
            if (target) {
                guarded_slots.push_back({target_offset, target});
            } else if (preserve_snapshot_iat) {
                std::memcpy(&target, image.data() + target_offset,
                            sizeof(target));
                if (!IsCommittedExecutableImageAddress(process, target)) {
                    if (by_ordinal) {
                        std::fprintf(
                            stderr,
                            "snapshot import validation failed: descriptor=%zu thunk=%zu module=%s ordinal=%u target=0x%llX\n",
                            descriptor_index, thunk_index, module,
                            static_cast<unsigned>(ordinal),
                            static_cast<unsigned long long>(target));
                    } else {
                        std::fprintf(
                            stderr,
                            "snapshot import validation failed: descriptor=%zu thunk=%zu module=%s symbol=%s target=0x%llX\n",
                            descriptor_index, thunk_index, module, symbol,
                            static_cast<unsigned long long>(target));
                    }
                    return false;
                }
            } else {
                target = ResolveRemoteExport(pid, module, symbol);
            }
            if (!target)
                return false;
            std::memcpy(image.data() + target_offset, &target,
                        sizeof(target));
            ++patched;
        }
    }
    return false;
}

bool LoadPages(const std::string& directory, std::vector<Page>& pages) {
    std::ifstream manifest(directory + "manifest.csv");
    if (!manifest)
        return false;
    std::string line;
    std::getline(manifest, line);
    while (std::getline(manifest, line)) {
        const auto fields = Csv(line);
        if (fields.size() < 7)
            continue;
        pages.push_back({std::strtoull(fields[0].c_str(), nullptr, 16),
                         fields[6], 0});
    }
    return pages.size() == 65;
}

bool RunTlsCallbacks(HANDLE process, std::uint32_t tls_index,
                     std::uint64_t tls_block,
                     std::uint64_t tls_set_value) {
    std::vector<std::uint8_t> stub = {0x48, 0x83, 0xEC, 0x28};
    const auto emit64 = [&stub](std::uint64_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        stub.insert(stub.end(), bytes, bytes + sizeof(value));
    };
    stub.push_back(0xB9);
    const auto* index_bytes = reinterpret_cast<const std::uint8_t*>(&tls_index);
    stub.insert(stub.end(), index_bytes, index_bytes + sizeof(tls_index));
    stub.push_back(0x48);
    stub.push_back(0xBA);
    emit64(tls_block);
    stub.push_back(0x48);
    stub.push_back(0xB8);
    emit64(tls_set_value);
    stub.push_back(0xFF);
    stub.push_back(0xD0);
    stub.push_back(0x85);
    stub.push_back(0xC0);
    stub.push_back(0x0F);
    stub.push_back(0x84);
    const auto failure_displacement = stub.size();
    stub.insert(stub.end(), 4, 0);
    for (const std::uint32_t rva : {0x49C3FCu, 0x4EFF80u, 0x49C464u}) {
        stub.push_back(0x48);
        stub.push_back(0xB9);
        emit64(kImageBase);
        const std::uint8_t reason[] = {0xBA, 0x01, 0x00, 0x00, 0x00,
                                       0x45, 0x33, 0xC0,
                                       0x48, 0xB8};
        stub.insert(stub.end(), std::begin(reason), std::end(reason));
        emit64(kImageBase + rva);
        stub.push_back(0xFF);
        stub.push_back(0xD0);
    }
    const std::uint8_t finish[] = {0xB8, 0x01, 0x00, 0x00, 0x00,
                                    0x48, 0x83, 0xC4, 0x28, 0xC3};
    stub.insert(stub.end(), std::begin(finish), std::end(finish));
    const auto failure = stub.size();
    const std::uint8_t failed[] = {0x33, 0xC0, 0x48, 0x83,
                                   0xC4, 0x28, 0xC3};
    stub.insert(stub.end(), std::begin(failed), std::end(failed));
    const auto relative = static_cast<std::int32_t>(
        failure - (failure_displacement + sizeof(std::int32_t)));
    std::memcpy(stub.data() + failure_displacement, &relative,
                sizeof(relative));
    void* remote_stub = VirtualAllocEx(process, nullptr, 0x1000,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!remote_stub ||
        !Write(process, reinterpret_cast<std::uint64_t>(remote_stub),
               stub.data(), stub.size()))
        return false;
    FlushInstructionCache(process, remote_stub, stub.size());
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub), nullptr, 0,
        nullptr);
    if (!thread)
        return false;
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD result = 0;
    GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    if (wait != WAIT_OBJECT_0 || result == 0)
        return false;
    const std::uint64_t zero_callbacks[3] = {0, 0, 0};
    return Write(process, kImageBase + 0x590340, zero_callbacks,
                 sizeof(zero_callbacks));
}

} 

int main(int argc, char** argv) {
    if (argc < 5 || (argc > 6 && argc != 8)) {
        std::fprintf(stderr,
                     "usage: manual_map_kirkware_active.exe <pid> <mapped-active-image> <handoff-blob> <proxy-bundle-dir> [reconstructed-bss] [bss-mask swap-module]\n");
        return 2;
    }
    const DWORD pid = std::strtoul(argv[1], nullptr, 10);
    auto image = ReadBytes(argv[2]);
    auto blob = ReadBytes(argv[3]);
    std::string bundle = argv[4];
    if (!bundle.empty() && bundle.back() != '\\' && bundle.back() != '/')
        bundle.push_back('\\');
    if (image.size() < kImageSize || blob.size() != 0x30240) {
        std::fprintf(stderr, "invalid image or handoff blob\n");
        return 10;
    }
    image.resize(kImageSize);
    const auto original_image = image;
    const bool swap_mode = argc == 8;
    if (argc == 6) {
        const auto reconstructed_bss = ReadBytes(argv[5]);
        if (reconstructed_bss.size() != 0xB82000 - 0x7E3D20) {
            std::fprintf(stderr, "invalid reconstructed BSS\n");
            return 11;
        }
        std::memcpy(image.data() + 0x7E3D20, reconstructed_bss.data(),
                    reconstructed_bss.size());
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
                                     PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ |
                                     PROCESS_SUSPEND_RESUME,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 12;
    }
    ProcessSuspension process_suspension(process);
    std::size_t mapped_image_size = kImageSize;
    if (swap_mode) {
        if (!process_suspension.Suspend()) {
            std::fprintf(stderr, "swap process suspension failed\n");
            CloseHandle(process);
            return 13;
        }
        const auto loaded_base = RemoteModuleBase(pid, argv[7]);
        const bool fixed_image = ValidateBootstrapImage(process);
        if (!fixed_image && loaded_base && loaded_base != kImageBase) {
            std::fprintf(
                stderr,
                "swap bootstrap validation failed: listed=0x%llX fixed=%u\n",
                static_cast<unsigned long long>(loaded_base),
                fixed_image ? 1u : 0u);
            CloseHandle(process);
            return 14;
        }
        if (!fixed_image) {
            std::fprintf(
                stderr,
                "swap bootstrap validation failed: listed=0x%llX fixed=%u\n",
                static_cast<unsigned long long>(loaded_base), 0u);
            CloseHandle(process);
            return 15;
        }
        mapped_image_size = kImageSize;
        image.resize(mapped_image_size, 0);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(kImageBase),
                               image.data(), image.size(), &read) ||
            read != image.size()) {
            std::fprintf(stderr, "swap snapshot failed: %lu\n", GetLastError());
            CloseHandle(process);
            return 16;
        }
        const auto reconstructed_bss = ReadBytes(argv[5]);
        auto bss_mask = std::string(argv[6]) == "-"
                            ? std::vector<std::uint8_t>(
                                  reconstructed_bss.size(), 0)
                            : ReadBytes(argv[6]);
        if (reconstructed_bss.size() != 0xB82000 - 0x7E3D20 ||
            bss_mask.size() != reconstructed_bss.size()) {
            std::fprintf(stderr, "swap BSS reconstruction inputs are invalid\n");
            CloseHandle(process);
            return 17;
        }
        for (std::size_t index = 0; index < reconstructed_bss.size(); ++index) {
            if (bss_mask[index])
                image[0x7E3D20 + index] = reconstructed_bss[index];
        }
        std::memcpy(image.data(), original_image.data(), 0x1000);
        std::memcpy(image.data() + 0xD50A20,
                    original_image.data() + 0xD50A20,
                    0xD51020 - 0xD50A20);
        for (const std::uint32_t rva :
             {0xC1D951u, 0xC1D959u, 0xC1D96Du, 0xC1D9D6u}) {
            std::memcpy(image.data() + rva, original_image.data() + rva, 8);
        }
        if (!PatchRemoteLoaderEntry(process)) {
            std::fprintf(stderr, "swap loader entry patch failed\n");
            CloseHandle(process);
            return 18;
        }
        const auto unmap_section = reinterpret_cast<NtUnmapViewOfSectionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                           "NtUnmapViewOfSection"));
        const LONG unmap_status =
            unmap_section
                ? unmap_section(process, reinterpret_cast<void*>(kImageBase))
                : static_cast<LONG>(0xC0000001u);
        if (unmap_status < 0) {
            std::fprintf(stderr, "swap unmap failed: 0x%08lX\n",
                         static_cast<unsigned long>(unmap_status));
            CloseHandle(process);
            return 19;
        }
    }
    std::memset(image.data() + 0x7E5CD0, 0, 0x400);
    const std::uint32_t empty_lowio_table_count = 0;
    std::memcpy(image.data() + 0x7E60D0, &empty_lowio_table_count,
                sizeof(empty_lowio_table_count));
    const std::uint64_t empty_vector[3] = {0, 0, 0};
    std::memcpy(image.data() + 0x7CE8A0, empty_vector, sizeof(empty_vector));
    const std::uint64_t empty_compression_map_count = 0;
    std::memcpy(image.data() + 0xB69758, &empty_compression_map_count,
                sizeof(empty_compression_map_count));
    const std::uint8_t network_expected[] = {
        0xE8, 0x1B, 0x2A, 0x3C, 0x00};
    const std::uint8_t late_auth_original_expected[] = {
        0x0F, 0x84, 0xB0, 0x00, 0x00, 0x00};
    const std::uint8_t freshness_expected[] = {0xCD, 0x29};
    const std::uint8_t periodic_expected[] = {
        0xE8, 0x55, 0xF8, 0xFF, 0xFF};
    const std::uint8_t render_cold_expected[] = {0x45, 0x33};
    constexpr std::array<std::uint32_t, 2> traversal_child_check_rvas{
        0x349544, 0x3495B5};
    constexpr std::array<std::uint8_t, 3> traversal_child_check_expected{
        0x48, 0x85, 0xC9};
    constexpr std::array<std::uint8_t, 3> traversal_child_check_patch{
        0x83, 0xF9, 0xFF};
    std::array<std::uint8_t,
               kirkware::kWatermarkDefaultEntryOriginal.size()>
        watermark_default_patch{};
    std::array<std::uint8_t,
               kirkware::kRendererStateVmHandoffOriginal.size()>
        renderer_state_exit_patch{};
    std::array<std::uint8_t, 7> scoped_marker_patch{};
    std::array<std::uint8_t, 7> scoped_phase_patch{};
    std::array<std::uint8_t, 6> scoped_decision_patch{};
    std::array<std::uint8_t, 6> scoped_accumulator_patch{};
    std::array<std::uint8_t, 10> scoped_e9_source_patch{};
    std::array<std::uint8_t, 10> scoped_gate2_source_patch{};
    std::array<std::uint8_t, kirkware::kScopedEarlyStubSize>
        scoped_stub{};
    if (!kirkware::BuildScopedEarlyAuthPatch(
            scoped_marker_patch, scoped_phase_patch,
            scoped_decision_patch, scoped_accumulator_patch,
            scoped_e9_source_patch,
            scoped_gate2_source_patch,
            scoped_stub) ||
        !kirkware::BuildWatermarkDefaultEntryPatch(
            watermark_default_patch) ||
        !kirkware::BuildRendererStateVmExitPatch(
            renderer_state_exit_patch)) {
        std::fprintf(stderr, "scoped early auth patch build failed\n");
        CloseHandle(process);
        return 20;
    }
    if (std::memcmp(image.data() + 0xE8150, network_expected,
                    sizeof(network_expected)) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedEarlyMarkerEntryRva,
            kirkware::kScopedEarlyMarkerOriginal.data(),
            kirkware::kScopedEarlyMarkerOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedEarlyPhaseEntryRva,
            kirkware::kScopedEarlyPhaseOriginal.data(),
            kirkware::kScopedEarlyPhaseOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedEarlyDecisionEntryRva,
            kirkware::kScopedEarlyDecisionOriginal.data(),
            kirkware::kScopedEarlyDecisionOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedAccumulatorEntryRva,
            kirkware::kScopedAccumulatorOriginal.data(),
            kirkware::kScopedAccumulatorOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedE9SourceEntryRva,
            kirkware::kScopedE9SourceOriginal.data(),
            kirkware::kScopedE9SourceOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kScopedGate2SourceEntryRva,
            kirkware::kScopedGate2SourceOriginal.data(),
            kirkware::kScopedGate2SourceOriginal.size()) != 0 ||
        std::memcmp(image.data() + 0xBC3F03,
                    late_auth_original_expected,
                    sizeof(late_auth_original_expected)) != 0 ||
        std::any_of(
            image.begin() + kirkware::kScopedEarlyCaveRva,
            image.begin() + kirkware::kScopedEarlyCaveRva +
                kirkware::kScopedEarlyStubSize,
            [](std::uint8_t value) { return value != 0; }) ||
        std::memcmp(image.data() + 0xDEF726, freshness_expected,
                    sizeof(freshness_expected)) != 0 ||
        std::memcmp(image.data() + 0x30DB86, periodic_expected,
                    sizeof(periodic_expected)) != 0 ||
        std::memcmp(image.data() + 0x1EC1F7, render_cold_expected,
                    sizeof(render_cold_expected)) != 0 ||
        !std::all_of(
            traversal_child_check_rvas.begin(),
            traversal_child_check_rvas.end(),
            [&](std::uint32_t rva) {
                return rva + traversal_child_check_expected.size() <=
                           image.size() &&
                       std::memcmp(
                           image.data() + rva,
                           traversal_child_check_expected.data(),
                           traversal_child_check_expected.size()) == 0;
            }) ||
        std::memcmp(
            image.data() + kirkware::kRendererStateVmHandoffRva,
            kirkware::kRendererStateVmHandoffOriginal.data(),
            kirkware::kRendererStateVmHandoffOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kRendererStateNativeExitRva,
            kirkware::kRendererStateNativeExitOriginal.data(),
            kirkware::kRendererStateNativeExitOriginal.size()) != 0 ||
        std::memcmp(
            image.data() + kirkware::kWatermarkDefaultEntryRva,
            kirkware::kWatermarkDefaultEntryOriginal.data(),
            kirkware::kWatermarkDefaultEntryOriginal.size()) != 0) {
        std::fprintf(stderr, "isolated patch validation failed\n");
        CloseHandle(process);
        return 21;
    }
    std::memcpy(
        image.data() + kirkware::kScopedEarlyMarkerEntryRva,
        scoped_marker_patch.data(), scoped_marker_patch.size());
    std::memcpy(
        image.data() + kirkware::kScopedEarlyPhaseEntryRva,
        scoped_phase_patch.data(), scoped_phase_patch.size());
    std::memcpy(
        image.data() + kirkware::kScopedEarlyDecisionEntryRva,
        scoped_decision_patch.data(), scoped_decision_patch.size());
    std::memcpy(
        image.data() + kirkware::kScopedAccumulatorEntryRva,
        scoped_accumulator_patch.data(), scoped_accumulator_patch.size());
    std::memcpy(
        image.data() + kirkware::kScopedE9SourceEntryRva,
        scoped_e9_source_patch.data(), scoped_e9_source_patch.size());
    std::memcpy(
        image.data() + kirkware::kScopedGate2SourceEntryRva,
        scoped_gate2_source_patch.data(),
        scoped_gate2_source_patch.size());
    std::memcpy(image.data() + kirkware::kScopedEarlyCaveRva,
                scoped_stub.data(), scoped_stub.size());
    image[0xDEF726] = 0x90;
    image[0xDEF727] = 0x90;
    std::memset(image.data() + 0x30DB86, 0x90,
                sizeof(periodic_expected));
    image[0x1EC1F7] = 0xEB;
    image[0x1EC1F8] = 0x17;
    for (const std::uint32_t rva : traversal_child_check_rvas) {
        std::memcpy(image.data() + rva,
                    traversal_child_check_patch.data(),
                    traversal_child_check_patch.size());
    }
    std::memcpy(
        image.data() + kirkware::kRendererStateVmHandoffRva,
        renderer_state_exit_patch.data(), renderer_state_exit_patch.size());
    std::memcpy(
        image.data() + kirkware::kWatermarkDefaultEntryRva,
        watermark_default_patch.data(), watermark_default_patch.size());
    if (!PatchAuxiliary(image, pid)) {
        CloseHandle(process);
        return 22;
    }
    image[0x7EA565] = 1;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_EXECUTE_READWRITE,
                                        static_cast<DWORD>(mapped_image_size >> 32),
                                        static_cast<DWORD>(mapped_image_size), nullptr);
    void* local_view = section ? MapViewOfFile(
                                     section,
                                     FILE_MAP_READ | FILE_MAP_WRITE |
                                         FILE_MAP_EXECUTE,
                                     0, 0, mapped_image_size)
                               : nullptr;
    const auto map_section = reinterpret_cast<NtMapViewOfSectionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtMapViewOfSection"));
    void* mapped = reinterpret_cast<void*>(kImageBase);
    SIZE_T mapped_size = mapped_image_size;
    const LONG map_status =
        section && local_view && map_section
            ? map_section(section, process, &mapped, 0, 0, nullptr,
                          &mapped_size, 2, 0, PAGE_EXECUTE_READWRITE)
            : static_cast<LONG>(0xC0000001u);
    if (map_status < 0 || reinterpret_cast<std::uint64_t>(mapped) != kImageBase) {
        std::fprintf(stderr,
                     "preferred section mapping failed: status=0x%08lX base=0x%llX\n",
                     static_cast<unsigned long>(map_status),
                     static_cast<unsigned long long>(
                         reinterpret_cast<std::uint64_t>(mapped)));
        CloseHandle(process);
        return static_cast<std::uint32_t>(map_status) == 0xC0000018u
                   ? 23
                   : 24;
    }
    void* remote_blob = VirtualAllocEx(process, nullptr, blob.size(),
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_blob ||
        !Write(process, reinterpret_cast<std::uint64_t>(remote_blob), blob.data(),
               blob.size())) {
        std::fprintf(stderr, "handoff allocation/write failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 25;
    }
    const std::uint64_t blob_address =
        reinterpret_cast<std::uint64_t>(remote_blob);
    std::memcpy(image.data() + 0xB6BC88, &blob_address, sizeof(blob_address));

    const auto wsa_set_last_error = ResolveRemoteExport(
        pid, "WS2_32.dll", MAKEINTRESOURCEA(112));
    kirkware::NetworkGuard network_guard{};
    if (!kirkware::install_network_guard(
            process, wsa_set_last_error, network_guard)) {
        std::fprintf(stderr, "payload-local network guard installation failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 26;
    }
    std::size_t import_count = 0;
    std::vector<kirkware::GuardedImportSlot> guarded_slots;
    if (!PatchFormalImports(image, process, pid, swap_mode, network_guard,
                            guarded_slots, import_count)) {
        std::fprintf(stderr, "formal import reconstruction failed after %zu slots\n",
                     import_count);
        VirtualFreeEx(process,
                      reinterpret_cast<void*>(network_guard.remote_page), 0,
                      MEM_RELEASE);
        CloseHandle(process);
        return 27;
    }
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::uint64_t unwind_result = 1;
    if (swap_mode) {
        std::memcpy(&tls_index, image.data() + kTlsIndexRva,
                    sizeof(tls_index));
    } else {
        const auto tls_alloc = ResolveRemoteExport(pid, "kernel32.dll",
                                                    "TlsAlloc");
        std::uint64_t tls_index_result = TLS_OUT_OF_INDEXES;
        if (!tls_alloc ||
            !RemoteCall3(process, tls_alloc, 0, 0, 0,
                         tls_index_result)) {
            std::fprintf(stderr, "target TLS index allocation failed\n");
            CloseHandle(process);
            return 28;
        }
        tls_index = static_cast<DWORD>(tls_index_result);
    }
    if (tls_index == TLS_OUT_OF_INDEXES) {
        std::fprintf(stderr, "target TLS index is invalid\n");
        CloseHandle(process);
        return 29;
    }
    std::memcpy(image.data() + kTlsIndexRva, &tls_index,
                sizeof(tls_index));
    std::memcpy(local_view, image.data(), image.size());
    MemoryBarrier();
    std::array<std::uint8_t, sizeof(periodic_expected)> periodic_readback{};
    SIZE_T periodic_read = 0;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(kImageBase + 0x30DB86),
            periodic_readback.data(), periodic_readback.size(),
            &periodic_read) ||
        periodic_read != periodic_readback.size() ||
        !std::all_of(periodic_readback.begin(), periodic_readback.end(),
                     [](std::uint8_t value) { return value == 0x90; })) {
        std::fprintf(stderr, "periodic call isolation readback failed\n");
        CloseHandle(process);
        return 30;
    }
    if (!kirkware::seal_network_guard_iat_page(process, kImageBase)) {
        std::fprintf(stderr, "network guard IAT sealing failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 31;
    }
    DWORD old_protect = 0;
    if (!VirtualProtectEx(process,
                          reinterpret_cast<void*>(kImageBase + 0xBB8000),
                          0x1000, PAGE_READONLY, &old_protect)) {
        std::fprintf(stderr, "readonly page protection failed: %lu\n",
                     GetLastError());
        CloseHandle(process);
        return 32;
    }
    FlushInstructionCache(process, reinterpret_cast<void*>(kImageBase),
                          image.size());
    if (!swap_mode) {
        const auto rtl_add = ResolveRemoteExport(pid, "ntdll.dll",
                                                  "RtlAddFunctionTable");
        unwind_result = 0;
        if (!rtl_add ||
            !RemoteCall3(process, rtl_add, kImageBase + kExceptionRva,
                         kExceptionCount, kImageBase, unwind_result) ||
            (unwind_result & 0xFFu) == 0) {
            std::fprintf(stderr, "RtlAddFunctionTable failed\n");
            CloseHandle(process);
            return 33;
        }
    }
    if (!kirkware::verify_network_guard_page(
            process, wsa_set_last_error, network_guard) ||
        !kirkware::verify_guarded_import_slots(
            process, kImageBase, guarded_slots)) {
        std::fprintf(stderr, "payload-local network guard verification failed\n");
        CloseHandle(process);
        return 34;
    }
#if defined(KIRKWARE_UNLINK_ACTIVE_IMAGE)
    const auto loader_entry = FindRemoteLoaderEntry(process);
    if (!loader_entry ||
        !UnlinkRemotePublicLoaderLists(process, loader_entry)) {
        std::fprintf(stderr, "active-image public loader unlink failed\n");
        CloseHandle(process);
        return 35;
    }
#endif
    const auto client_base = RemoteModuleBase(pid, "client.dll");
    kirkware::BoneAccessCompatResult bone_compat{};
    if (!client_base ||
        !kirkware::InstallKirkwareBoneAccessCompat(
            process, client_base, kImageBase, kImageSize, bone_compat)) {
        std::fprintf(stderr,
                     "bone access compatibility failed: %s win32=%lu\n",
                     kirkware::BoneAccessCompatStatusText(
                         bone_compat.status),
                     static_cast<unsigned long>(bone_compat.win32_error));
        CloseHandle(process);
        return 36;
    }
    if (!process_suspension.Resume()) {
        std::fprintf(stderr, "swap process resume failed\n");
        CloseHandle(process);
        return 37;
    }
    std::printf("pid=%lu image=0x%llX blob=0x%llX imports=%zu mapped=1 swap=%u tls=%lu unwind=0x%llX network_guarded=%zu guard_page=0x%llX iat_sealed=1 guard_verified=1"
#if defined(KIRKWARE_UNLINK_ACTIVE_IMAGE)
                " peb_public_unlinked=1"
#endif
                " bone_compat=1 setup=0x%X push=0x%X pop=0x%X"
                "\n",
                 pid, static_cast<unsigned long long>(kImageBase),
                 static_cast<unsigned long long>(blob_address), import_count,
                 swap_mode ? 1u : 0u, tls_index,
                 static_cast<unsigned long long>(unwind_result),
                 guarded_slots.size(),
                 static_cast<unsigned long long>(network_guard.remote_page),
                 bone_compat.setup_bones_rva,
                 bone_compat.push_bone_access_rva,
                 bone_compat.pop_bone_access_rva);
    UnmapViewOfFile(local_view);
    CloseHandle(section);
    CloseHandle(process);
    return 0;
}
