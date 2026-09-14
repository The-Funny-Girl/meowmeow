#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cstdint>

struct RemoteLoadContext {
    std::uint64_t path;
    std::uint64_t load_library;
    std::uint64_t get_last_error;
    std::uint64_t module;
    std::uint32_t error;
    std::uint32_t reserved;
};

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: inject_library.exe <pid> <dll>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    wchar_t path[MAX_PATH]{};
    if (GetFullPathNameW(argv[2], MAX_PATH, path, nullptr) == 0) {
        std::fwprintf(stderr, L"GetFullPathNameW failed: %lu\n", GetLastError());
        return 1;
    }
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        std::fwprintf(stderr, L"DLL not found: %ls\n", path);
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
                                     PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, pid);
    if (process == nullptr) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    const SIZE_T path_bytes = (std::wcslen(path) + 1) * sizeof(wchar_t);
    static const std::uint8_t stub[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B,
        0x0B, 0xFF, 0x53, 0x08, 0x48, 0x89, 0x43, 0x18, 0xFF, 0x53,
        0x10, 0x89, 0x43, 0x20, 0x48, 0x8B, 0x43, 0x18, 0x48, 0x83,
        0xC4, 0x20, 0x5B, 0xC3};
    const SIZE_T context_offset = (path_bytes + 15) & ~SIZE_T(15);
    const SIZE_T stub_offset = context_offset + sizeof(RemoteLoadContext);
    const SIZE_T allocation_size = stub_offset + sizeof(stub);
    auto* remote = static_cast<std::uint8_t*>(VirtualAllocEx(
        process, nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (remote == nullptr) {
        std::fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    auto* remote_path = remote;
    auto* remote_context = remote + context_offset;
    auto* remote_stub = remote + stub_offset;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_path, path, path_bytes, &written) ||
        written != path_bytes) {
        std::fwprintf(stderr, L"WriteProcessMemory failed: %lu\n",
                      GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    const auto load_library =
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    const auto get_last_error =
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLastError");
    if (load_library == nullptr || get_last_error == nullptr) {
        std::fwprintf(stderr, L"GetProcAddress failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    RemoteLoadContext context{};
    context.path = reinterpret_cast<std::uint64_t>(remote_path);
    context.load_library = reinterpret_cast<std::uint64_t>(load_library);
    context.get_last_error = reinterpret_cast<std::uint64_t>(get_last_error);
    if (!WriteProcessMemory(process, remote_context, &context, sizeof(context),
                            &written) ||
        written != sizeof(context) ||
        !WriteProcessMemory(process, remote_stub, stub, sizeof(stub), &written) ||
        written != sizeof(stub)) {
        std::fwprintf(stderr, L"WriteProcessMemory failed: %lu\n",
                      GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub), remote_context,
        0, nullptr);
    if (thread == nullptr) {
        std::fwprintf(stderr, L"CreateRemoteThread failed: %lu\n",
                      GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD result = 0;
    GetExitCodeThread(thread, &result);
    SIZE_T read = 0;
    ReadProcessMemory(process, remote_context, &context, sizeof(context),
                      &read);
    std::wprintf(L"pid=%lu dll=%ls wait=%lu module=0x%llX "
                 L"last_error=%lu thread_low32=0x%08lX\n",
                 pid, path, wait,
                 static_cast<unsigned long long>(context.module), context.error,
                 result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0 && context.module != 0 ? 0 : 1;
}
