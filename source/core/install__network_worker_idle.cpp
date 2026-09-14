#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uint64_t kImageBase = 0x1E5DCC00000ull;
constexpr std::uint64_t kWorkerRva = 0x4A70C0ull;
constexpr std::uint64_t kSleepIatRva = 0x58F608ull;

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

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: install_kirkware_network_worker_idle.exe "
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
        return 1;
    }

    constexpr std::array<std::uint8_t, 12> expected{
        0x48, 0x8B, 0xD1, 0x33, 0xC9, 0xE9,
        0x36, 0x17, 0x00, 0x00, 0xCC, 0xCC,
    };
    std::array<std::uint8_t, expected.size()> current{};
    const auto worker = kImageBase + kWorkerRva;
    std::uint64_t sleep_address = 0;
    if (!Read(process, worker, current.data(), current.size()) ||
        current != expected ||
        !Read(process, kImageBase + kSleepIatRva, &sleep_address,
              sizeof(sleep_address)) ||
        !sleep_address) {
        std::fprintf(stderr,
                     "worker/IAT verification failed error=%lu sleep=0x%llX\n",
                     GetLastError(),
                     static_cast<unsigned long long>(sleep_address));
        CloseHandle(process);
        return 1;
    }
    MEMORY_BASIC_INFORMATION sleep_memory{};
    if (!VirtualQueryEx(process, reinterpret_cast<const void*>(sleep_address),
                        &sleep_memory, sizeof(sleep_memory)) ||
        sleep_memory.State != MEM_COMMIT ||
        (sleep_memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY)) == 0) {
        std::fprintf(stderr, "Sleep target is not executable\n");
        CloseHandle(process);
        return 1;
    }

    std::array<std::uint8_t, 23> idle_stub{
        0x48, 0x83, 0xEC, 0x28,
        0xB9, 0xFF, 0xFF, 0xFF, 0xFF,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0xEB, 0xED,
    };
    std::memcpy(idle_stub.data() + 11, &sleep_address, sizeof(sleep_address));
    auto* remote_stub = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!remote_stub ||
        !Write(process, reinterpret_cast<std::uint64_t>(remote_stub),
               idle_stub.data(), idle_stub.size())) {
        std::fprintf(stderr, "idle stub allocation/write failed=%lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }
    DWORD stub_old = 0;
    if (!VirtualProtectEx(process, remote_stub, 0x1000, PAGE_EXECUTE_READ,
                          &stub_old)) {
        std::fprintf(stderr, "idle stub protection failed=%lu\n",
                     GetLastError());
        CloseHandle(process);
        return 1;
    }

    std::array<std::uint8_t, 12> entry_patch{
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0,
    };
    const auto stub_address = reinterpret_cast<std::uint64_t>(remote_stub);
    std::memcpy(entry_patch.data() + 2, &stub_address, sizeof(stub_address));
    DWORD worker_old = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(worker),
                          entry_patch.size(), PAGE_EXECUTE_READWRITE,
                          &worker_old) ||
        !Write(process, worker, entry_patch.data(), entry_patch.size())) {
        std::fprintf(stderr, "worker patch failed=%lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    DWORD ignored = 0;
    VirtualProtectEx(process, reinterpret_cast<void*>(worker),
                     entry_patch.size(), worker_old, &ignored);
    FlushInstructionCache(process, reinterpret_cast<void*>(worker),
                          entry_patch.size());
    FlushInstructionCache(process, remote_stub, idle_stub.size());

    std::array<std::uint8_t, entry_patch.size()> verified{};
    if (!Read(process, worker, verified.data(), verified.size()) ||
        verified != entry_patch) {
        std::fprintf(stderr, "worker patch readback failed\n");
        CloseHandle(process);
        return 1;
    }
    std::printf("pid=%lu worker=0x%llX idle_stub=0x%llX sleep=0x%llX "
                "verified=1\n",
                pid, static_cast<unsigned long long>(worker),
                static_cast<unsigned long long>(stub_address),
                static_cast<unsigned long long>(sleep_address));
    CloseHandle(process);
    return 0;
}
