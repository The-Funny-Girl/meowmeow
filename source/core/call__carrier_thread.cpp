#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct CallContext {
    std::uint64_t target;
    std::uint64_t callback;
    std::uint64_t image_base;
    std::uint64_t tls_index;
    std::uint64_t rcx;
    std::uint64_t rdx;
    std::uint64_t r8;
    std::uint64_t result;
    std::uint64_t tls_block;
    std::uint64_t flag_before;
    std::uint64_t flag_after;
};

int main(int argc, char** argv) {
    if (argc < 3 || argc > 6)
        return 2;
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const std::uint64_t image_base = 0x1E5DCC00000ull;
    CallContext context{};
    context.target = std::strtoull(argv[2], nullptr, 0);
    if (argc > 3)
        context.rcx = std::strtoull(argv[3], nullptr, 0);
    if (argc > 4)
        context.rdx = std::strtoull(argv[4], nullptr, 0);
    if (argc > 5)
        context.r8 = std::strtoull(argv[5], nullptr, 0);
    context.callback = image_base + 0x49C3FC;
    context.image_base = image_base;
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
                                     PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION |
                                     PROCESS_VM_READ |
                                     PROCESS_VM_WRITE,
                                 FALSE, pid);
    if (!process)
        return 3;
    SIZE_T done = 0;
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const void*>(image_base +
                                                         0x7E4790),
                           &tls_index, sizeof(tls_index), &done) ||
        done != sizeof(tls_index) || tls_index == TLS_OUT_OF_INDEXES) {
        CloseHandle(process);
        return 4;
    }
    context.tls_index = tls_index;
    static constexpr std::uint8_t stub[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB,
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
        0x8B, 0x4B, 0x18, 0x48, 0x8B, 0x04, 0xC8,
        0x48, 0x89, 0x43, 0x40,
        0x0F, 0xB6, 0x48, 0x14, 0x48, 0x89, 0x4B, 0x48,
        0xC6, 0x40, 0x14, 0,
        0x48, 0x8B, 0x4B, 0x10, 0xBA, 2, 0, 0, 0,
        0x45, 0x33, 0xC0, 0xFF, 0x53, 0x08,
        0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0,
        0x8B, 0x4B, 0x18, 0x48, 0x8B, 0x04, 0xC8,
        0x0F, 0xB6, 0x48, 0x14, 0x48, 0x89, 0x4B, 0x50,
        0x80, 0xF9, 1, 0x75, 0x1A,
        0x48, 0x8B, 0x4B, 0x20, 0x48, 0x8B, 0x53, 0x28,
        0x4C, 0x8B, 0x43, 0x30, 0xFF, 0x13,
        0x48, 0x89, 0x43, 0x38, 0x33, 0xC0,
        0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
        0xB8, 1, 0, 0, 0, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
    };
    const SIZE_T context_size = (sizeof(context) + 15u) & ~SIZE_T(15u);
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, context_size + sizeof(stub),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!remote ||
        !WriteProcessMemory(process, remote, &context, sizeof(context),
                            &done) || done != sizeof(context) ||
        !WriteProcessMemory(process, remote + context_size, stub,
                            sizeof(stub), &done) || done != sizeof(stub)) {
        CloseHandle(process);
        return 5;
    }
    FlushInstructionCache(process, remote + context_size, sizeof(stub));
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote + context_size),
        remote, 0, nullptr);
    const DWORD wait = thread ? WaitForSingleObject(thread, 60000)
                              : WAIT_FAILED;
    DWORD exit_code = 0;
    if (thread)
        GetExitCodeThread(thread, &exit_code);
    done = 0;
    const bool read =
        ReadProcessMemory(process, remote, &context, sizeof(context),
                          &done) != FALSE && done == sizeof(context);
    std::printf("pid=%lu target=0x%llX tls=%llu block=0x%llX "
                "flag_before=%llu flag_after=%llu wait=%lu exit=%lu "
                "result=0x%llX read=%d\n",
                pid, static_cast<unsigned long long>(context.target),
                static_cast<unsigned long long>(context.tls_index),
                static_cast<unsigned long long>(context.tls_block),
                static_cast<unsigned long long>(context.flag_before),
                static_cast<unsigned long long>(context.flag_after), wait,
                exit_code, static_cast<unsigned long long>(context.result),
                read ? 1 : 0);
    if (thread)
        CloseHandle(thread);
    if (wait == WAIT_OBJECT_0)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return read && wait == WAIT_OBJECT_0 && exit_code == 0 &&
                   context.flag_after == 1
               ? 0
               : 1;
}
