#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uint64_t kImageBase = 0x1E5DCC00000ull;
constexpr std::array<std::uint64_t, 2> kInterfaceSlots{
    kImageBase + 0xB6BD40ull,
    kImageBase + 0xB6C700ull,
};
constexpr std::size_t kPageSize = 0x1000;
constexpr std::size_t kVtableOffset = 0x100;
constexpr std::size_t kVtableSlots = 64;
constexpr std::size_t kStubOffset = 0x400;
constexpr std::array<std::uint64_t, 3> kDisabledEntrypoints{
    kImageBase + 0x1C2E20ull,
    kImageBase + 0x1C38A0ull,
    kImageBase + 0x2883E0ull,
};
constexpr std::array<std::uint8_t, 16> kCloudCallbackProlog{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55,
};
constexpr std::array<std::uint8_t, 16> kProfileRequestProlog{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55,
    0x53, 0x48, 0x8D, 0x68, 0x88, 0x48, 0x81, 0xEC,
};

struct BytePatch {
    std::uint64_t address;
    std::size_t length;
    std::array<std::uint8_t, 6> expected;
    std::array<std::uint8_t, 6> replacement;
};

constexpr std::array<BytePatch, 6> kCloudCallPatches{{
    {kImageBase + 0x2C31B7ull, 3,
     {0x41, 0xFF, 0xD2, 0, 0, 0}, {0x31, 0xC0, 0x90, 0, 0, 0}},
    {kImageBase + 0x1C2AE9ull, 3,
     {0xFF, 0x50, 0x38, 0, 0, 0}, {0x31, 0xC0, 0x90, 0, 0, 0}},
    {kImageBase + 0x1C2D77ull, 6,
     {0xFF, 0x90, 0xC0, 0x00, 0x00, 0x00},
     {0x31, 0xC0, 0x90, 0x90, 0x90, 0x90}},
    {kImageBase + 0x2C9940ull, 2,
     {0xFF, 0x10, 0, 0, 0, 0}, {0x31, 0xC0, 0, 0, 0, 0}},
    {kImageBase + 0x2C995Cull, 3,
     {0xFF, 0x50, 0x38, 0, 0, 0}, {0x31, 0xC0, 0x90, 0, 0, 0}},
    {kImageBase + 0x2C9BA3ull, 6,
     {0xFF, 0x90, 0xC0, 0x00, 0x00, 0x00},
     {0x31, 0xC0, 0x90, 0x90, 0x90, 0x90}},
}};

DWORD guarded_pid = 0;
std::uint64_t guarded_page = 0;

bool Read(HANDLE process, std::uint64_t address, void* output,
          std::size_t size) {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             output, size, &done) != FALSE && done == size;
}

bool Write(HANDLE process, std::uint64_t address, const void* input,
           std::size_t size) {
    SIZE_T done = 0;
    return WriteProcessMemory(process, reinterpret_cast<void*>(address), input,
                              size, &done) != FALSE && done == size;
}

bool ExecutableReadOnly(DWORD protection) {
    const DWORD value = protection & 0xff;
    return value == PAGE_EXECUTE || value == PAGE_EXECUTE_READ;
}

bool PrepareGuardPage(HANDLE process, DWORD pid) {
    if (guarded_page != 0 && guarded_pid == pid)
        return true;
    std::array<std::uint8_t, kPageSize> page{};
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, page.size(), MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!remote)
        return false;
    const auto page_address = reinterpret_cast<std::uint64_t>(remote);
    const auto vtable_address = page_address + kVtableOffset;
    const auto stub_address = page_address + kStubOffset;
    std::memcpy(page.data(), &vtable_address, sizeof(vtable_address));
    for (std::size_t index = 0; index < kVtableSlots; ++index)
        std::memcpy(page.data() + kVtableOffset + index * sizeof(stub_address),
                    &stub_address, sizeof(stub_address));
    constexpr std::array<std::uint8_t, 3> stub{0x33, 0xC0, 0xC3};
    std::memcpy(page.data() + kStubOffset, stub.data(), stub.size());
    if (!Write(process, page_address, page.data(), page.size()))
        return false;
    DWORD old_protection = 0;
    if (!VirtualProtectEx(process, remote, page.size(), PAGE_EXECUTE_READ,
                          &old_protection))
        return false;
    if (!FlushInstructionCache(process, remote, page.size()))
        return false;
    guarded_pid = pid;
    guarded_page = page_address;
    return true;
}

bool WriteInterfaceSlot(HANDLE process, std::uint64_t slot,
                        std::uint64_t value) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQueryEx(process, reinterpret_cast<const void*>(slot), &memory,
                        sizeof(memory)) ||
        memory.State != MEM_COMMIT)
        return false;
    DWORD old_protection = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(slot),
                          sizeof(value), PAGE_READWRITE, &old_protection))
        return false;
    const bool written = Write(process, slot, &value, sizeof(value));
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(slot),
                         sizeof(value), old_protection, &ignored) != FALSE;
    return written && restored;
}

bool PatchEntrypoint(HANDLE process, std::uint64_t address,
                     const std::array<std::uint8_t, 16>& expected) {
    std::array<std::uint8_t, 16> current{};
    if (!Read(process, address, current.data(), current.size()))
        return false;
    const bool original = current == expected;
    const bool already_patched =
        current[0] == 0xC3 &&
        std::equal(current.begin() + 1, current.end(), expected.begin() + 1);
    if (!original && !already_patched)
        return false;
    if (already_patched)
        return true;
    constexpr std::uint8_t return_instruction = 0xC3;
    DWORD old_protection = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address), 1,
                          PAGE_EXECUTE_READWRITE, &old_protection))
        return false;
    const bool written =
        Write(process, address, &return_instruction,
              sizeof(return_instruction));
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(address), 1,
                         old_protection, &ignored) != FALSE;
    const bool flushed =
        FlushInstructionCache(process, reinterpret_cast<void*>(address), 1) !=
        FALSE;
    return written && restored && flushed;
}

bool PatchInstruction(HANDLE process, const BytePatch& patch) {
    std::array<std::uint8_t, 6> current{};
    if (!Read(process, patch.address, current.data(), patch.length))
        return false;
    const bool original =
        std::equal(current.begin(), current.begin() + patch.length,
                   patch.expected.begin());
    const bool already_patched =
        std::equal(current.begin(), current.begin() + patch.length,
                   patch.replacement.begin());
    if (!original && !already_patched)
        return false;
    if (already_patched)
        return true;
    DWORD old_protection = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(patch.address),
                          patch.length, PAGE_EXECUTE_READWRITE,
                          &old_protection))
        return false;
    const bool written = Write(process, patch.address,
                               patch.replacement.data(), patch.length);
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(patch.address),
                         patch.length, old_protection, &ignored) != FALSE;
    const bool flushed =
        FlushInstructionCache(process,
                              reinterpret_cast<void*>(patch.address),
                              patch.length) != FALSE;
    return written && restored && flushed;
}

bool Verify(HANDLE process) {
    if (guarded_page == 0)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQueryEx(process,
                        reinterpret_cast<const void*>(guarded_page), &memory,
                        sizeof(memory)) ||
        memory.State != MEM_COMMIT || !ExecutableReadOnly(memory.Protect))
        return false;
    const auto expected_vtable = guarded_page + kVtableOffset;
    const auto expected_stub_address = guarded_page + kStubOffset;
    std::uint64_t actual_vtable = 0;
    if (!Read(process, guarded_page, &actual_vtable,
              sizeof(actual_vtable)) ||
        actual_vtable != expected_vtable)
        return false;
    std::array<std::uint64_t, kVtableSlots> vtable{};
    if (!Read(process, expected_vtable, vtable.data(), sizeof(vtable)))
        return false;
    for (const auto entry : vtable) {
        if (entry != expected_stub_address)
            return false;
    }
    constexpr std::array<std::uint8_t, 3> expected_stub{0x33, 0xC0, 0xC3};
    std::array<std::uint8_t, expected_stub.size()> actual_stub{};
    if (!Read(process, expected_stub_address, actual_stub.data(),
              actual_stub.size()) ||
        actual_stub != expected_stub)
        return false;
    for (const auto slot : kInterfaceSlots) {
        std::uint64_t value = 0;
        if (!Read(process, slot, &value, sizeof(value)) ||
            value != guarded_page)
            return false;
    }
    for (std::size_t index = 0; index < kDisabledEntrypoints.size(); ++index) {
        const auto& expected =
            index < 2 ? kCloudCallbackProlog : kProfileRequestProlog;
        std::array<std::uint8_t, 16> current{};
        if (!Read(process, kDisabledEntrypoints[index], current.data(),
                  current.size()) ||
            current[0] != 0xC3 ||
            !std::equal(current.begin() + 1, current.end(),
                        expected.begin() + 1))
            return false;
    }
    for (const auto& patch : kCloudCallPatches) {
        std::array<std::uint8_t, 6> current{};
        if (!Read(process, patch.address, current.data(), patch.length) ||
            !std::equal(current.begin(), current.begin() + patch.length,
                        patch.replacement.begin()))
            return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: install_kirkware_steam_offline_guard.exe "
                     "<pid>\n");
        return 2;
    }
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                     PROCESS_VM_WRITE,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed=%lu\n", GetLastError());
        return 3;
    }
    bool success = PrepareGuardPage(process, pid);
    if (!success) {
        CloseHandle(process);
        return 4;
    }
    for (std::size_t index = 0; index < kDisabledEntrypoints.size();
         ++index) {
        const auto& expected =
            index < 2 ? kCloudCallbackProlog : kProfileRequestProlog;
        if (!PatchEntrypoint(process, kDisabledEntrypoints[index], expected)) {
            CloseHandle(process);
            return 5;
        }
    }
    for (const auto& patch : kCloudCallPatches) {
        if (!PatchInstruction(process, patch)) {
            CloseHandle(process);
            return 6;
        }
    }
    for (const auto slot : kInterfaceSlots) {
        if (!WriteInterfaceSlot(process, slot, guarded_page)) {
            CloseHandle(process);
            return 7;
        }
    }
    if (!Verify(process)) {
        std::fprintf(stderr, "Steam offline guard failed=%lu\n",
                     GetLastError());
        CloseHandle(process);
        return 8;
    }
    std::printf("pid=%lu steam_offline_guarded=%zu disabled_entries=%zu "
                "disabled_calls=%zu guard_page=0x%llX verified=1\n",
                pid, kInterfaceSlots.size(), kDisabledEntrypoints.size(),
                kCloudCallPatches.size(),
                static_cast<unsigned long long>(guarded_page));
    CloseHandle(process);
    return 0;
}
