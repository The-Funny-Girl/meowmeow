#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr,
                     "usage: install_kirkware_locale.exe <pid> <image-hex> <mbcinfo-file>\n");
        return 2;
    }
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const auto image = std::strtoull(argv[2], nullptr, 0);
    std::ifstream input(argv[3], std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    if (!pid || !image || bytes.size() < 0x220)
        return 2;
    bytes.resize(0x220);
    const std::uint32_t reference_count = 0x7fffffff;
    std::memcpy(bytes.data(), &reference_count, sizeof(reference_count));
    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ |
                                     PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process)
        return 3;
    void* remote = VirtualAllocEx(process, nullptr, bytes.size(),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T done = 0;
    const auto remote_address = reinterpret_cast<std::uint64_t>(remote);
    bool ok = remote &&
                    WriteProcessMemory(process, remote, bytes.data(), bytes.size(),
                                       &done) != FALSE &&
                    done == bytes.size() &&
                    WriteProcessMemory(process,
                                       reinterpret_cast<void*>(image + 0x6F9400),
                                       &remote_address, sizeof(remote_address),
                                       &done) != FALSE &&
                    done == sizeof(remote_address);
    if (ok && argc == 5 && std::strcmp(argv[4], "reset") == 0) {
        const std::uint32_t startup_state = 0;
        ok = WriteProcessMemory(
                 process, reinterpret_cast<void*>(image + 0x7E41D0),
                 &startup_state, sizeof(startup_state), &done) != FALSE &&
             done == sizeof(startup_state);
    }
    std::printf("pid=%lu locale=0x%llX global=0x%llX ok=%u\n", pid,
                static_cast<unsigned long long>(remote_address),
                static_cast<unsigned long long>(image + 0x6F9400),
                ok ? 1u : 0u);
    CloseHandle(process);
    return ok ? 0 : 1;
}
