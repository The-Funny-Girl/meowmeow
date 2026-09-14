#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<std::uint8_t> ReadFile(const char* path) {
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

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: install_kirkware_context.exe <pid> <image-hex> <envelope> envelope\n");
        return 2;
    }

    const DWORD pid = std::strtoul(argv[1], nullptr, 10);
    const std::uint64_t image = std::strtoull(argv[2], nullptr, 16);
    if (image != 0x000001E5DCC00000ull ||
        std::string(argv[4]) != "envelope") {
        std::fprintf(stderr, "invalid exact-dual envelope target\n");
        return 1;
    }
    constexpr std::uint64_t target = 0x000001E5D2490000ull;

    auto page = ReadFile(argv[3]);
    if (page.size() != 0x30298) {
        std::fprintf(stderr, "invalid envelope size\n");
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_READ | PROCESS_VM_WRITE,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    void* remote = VirtualAllocEx(process, reinterpret_cast<void*>(target),
                                  page.size(), MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
    if (!remote) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(target), &mbi,
                            sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            reinterpret_cast<std::uint64_t>(mbi.BaseAddress) > target ||
            reinterpret_cast<std::uint64_t>(mbi.BaseAddress) + mbi.RegionSize <
                target + page.size()) {
            std::fprintf(stderr, "exact allocation failed: %lu\n", GetLastError());
            CloseHandle(process);
            return 1;
        }
        remote = reinterpret_cast<void*>(target);
    }
    if (reinterpret_cast<std::uint64_t>(remote) != target) {
        std::fprintf(stderr, "allocation returned the wrong address\n");
        CloseHandle(process);
        return 1;
    }

    DWORD old_protect = 0;
    if (!VirtualProtectEx(process, remote, page.size(), PAGE_READWRITE,
                          &old_protect)) {
        std::fprintf(stderr, "VirtualProtectEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, page.data(), page.size(), &written) ||
        written != page.size()) {
        std::fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    std::vector<std::uint8_t> verify(page.size());
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, remote, verify.data(), verify.size(), &read) ||
        read != verify.size() || verify != page) {
        std::fprintf(stderr, "context verification failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    std::printf("pid=%lu image=0x%llX context=0x%llX verified=1\n", pid,
                static_cast<unsigned long long>(image),
                static_cast<unsigned long long>(target));
    CloseHandle(process);
    return 0;
}
