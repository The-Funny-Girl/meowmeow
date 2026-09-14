#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kirkware {

constexpr std::uint32_t kNetworkGuardWsaAccessDenied = 10013;
constexpr std::uint32_t kNetworkGuardDnsFailure = 11003;
constexpr std::size_t kExpectedGuardedImportCount = 16;
constexpr std::uint64_t kNetworkGuardIatPageRva = 0x58F000;
constexpr std::size_t kNetworkGuardIatPageSize = 0x1000;

enum class NetworkGuardResult : std::size_t {
    socket_error = 0,
    invalid_socket = 1,
    false_value = 2,
    dns_failure = 3,
    count = 4
};

struct NetworkGuard {
    std::uint64_t remote_page = 0;
    std::size_t page_size = 0;
    std::array<std::uint64_t,
               static_cast<std::size_t>(NetworkGuardResult::count)>
        targets{};
};

struct GuardedImportSlot {
    std::uint64_t image_rva = 0;
    std::uint64_t target = 0;
};

inline bool network_guard_equal_ascii(const char* left, const char* right) {
    if (!left || !right)
        return false;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return *left == *right;
}

inline bool network_guard_module_is(const char* module, const char* wanted) {
    if (!module)
        return false;
    const char* base = module;
    for (const char* cursor = module; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/')
            base = cursor + 1;
    }
    return network_guard_equal_ascii(base, wanted);
}

inline bool network_guard_symbol_is(const char* symbol, const char* wanted) {
    return network_guard_equal_ascii(symbol, wanted);
}

inline bool classify_guarded_import(const char* module, const char* symbol,
                                    bool by_ordinal, std::uint16_t ordinal,
                                    NetworkGuardResult& result) {
    if (network_guard_module_is(module, "ws2_32.dll")) {
        if (by_ordinal) {
            switch (ordinal) {
                case 1:
                case 23:
                    result = NetworkGuardResult::invalid_socket;
                    return true;
                case 2:
                case 4:
                case 13:
                case 16:
                case 17:
                case 19:
                case 20:
                    result = NetworkGuardResult::socket_error;
                    return true;
                default:
                    return false;
            }
        }
        if (network_guard_symbol_is(symbol, "WSASocketW")) {
            result = NetworkGuardResult::invalid_socket;
            return true;
        }
        if (network_guard_symbol_is(symbol, "WSARecv") ||
            network_guard_symbol_is(symbol, "WSASend") ||
            network_guard_symbol_is(symbol, "WSAIoctl")) {
            result = NetworkGuardResult::socket_error;
            return true;
        }
        if (network_guard_symbol_is(symbol, "getaddrinfo")) {
            result = NetworkGuardResult::dns_failure;
            return true;
        }
        return false;
    }
    if (network_guard_module_is(module, "mswsock.dll")) {
        if (by_ordinal) {
            if (ordinal == 1 || ordinal == 54) {
                result = NetworkGuardResult::false_value;
                return true;
            }
            if (ordinal == 55) {
                result = NetworkGuardResult::socket_error;
                return true;
            }
            return false;
        }
        if (!by_ordinal &&
            (network_guard_symbol_is(symbol, "AcceptEx") ||
             network_guard_symbol_is(symbol, "TransmitFile"))) {
            result = NetworkGuardResult::false_value;
            return true;
        }
    }
    return false;
}

inline std::uint64_t guarded_import_target(const NetworkGuard& guard,
                                           const char* module,
                                           const char* symbol,
                                           bool by_ordinal,
                                           std::uint16_t ordinal) {
    NetworkGuardResult result{};
    if (!classify_guarded_import(module, symbol, by_ordinal, ordinal, result))
        return 0;
    return guard.targets[static_cast<std::size_t>(result)];
}

inline void append_network_guard_u32(std::vector<std::uint8_t>& code,
                                     std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

inline void append_network_guard_u64(std::vector<std::uint8_t>& code,
                                     std::uint64_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

inline std::vector<std::uint8_t> make_network_guard_stub(
    std::uint64_t remote_wsa_set_last_error, std::uint32_t error,
    NetworkGuardResult result) {
    std::vector<std::uint8_t> code = {
        0x48, 0x83, 0xEC, 0x28,
        0xB9
    };
    append_network_guard_u32(code, error);
    code.push_back(0x48);
    code.push_back(0xB8);
    append_network_guard_u64(code, remote_wsa_set_last_error);
    code.push_back(0xFF);
    code.push_back(0xD0);
    switch (result) {
        case NetworkGuardResult::socket_error:
            code.push_back(0xB8);
            append_network_guard_u32(code, 0xFFFFFFFFu);
            break;
        case NetworkGuardResult::invalid_socket:
            code.push_back(0x48);
            code.push_back(0x83);
            code.push_back(0xC8);
            code.push_back(0xFF);
            break;
        case NetworkGuardResult::false_value:
            code.push_back(0x31);
            code.push_back(0xC0);
            break;
        case NetworkGuardResult::dns_failure:
            code.push_back(0xB8);
            append_network_guard_u32(code, kNetworkGuardDnsFailure);
            break;
        default:
            return {};
    }
    const std::uint8_t finish[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
    code.insert(code.end(), std::begin(finish), std::end(finish));
    return code;
}

inline bool install_network_guard(HANDLE process,
                                  std::uint64_t remote_wsa_set_last_error,
                                  NetworkGuard& guard) {
    if (!process || !remote_wsa_set_last_error)
        return false;
    constexpr std::size_t page_size = 0x1000;
    auto* remote = VirtualAllocEx(process, nullptr, page_size,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return false;
    std::array<std::vector<std::uint8_t>,
               static_cast<std::size_t>(NetworkGuardResult::count)>
        stubs;
    stubs[static_cast<std::size_t>(NetworkGuardResult::socket_error)] =
        make_network_guard_stub(remote_wsa_set_last_error,
                                kNetworkGuardWsaAccessDenied,
                                NetworkGuardResult::socket_error);
    stubs[static_cast<std::size_t>(NetworkGuardResult::invalid_socket)] =
        make_network_guard_stub(remote_wsa_set_last_error,
                                kNetworkGuardWsaAccessDenied,
                                NetworkGuardResult::invalid_socket);
    stubs[static_cast<std::size_t>(NetworkGuardResult::false_value)] =
        make_network_guard_stub(remote_wsa_set_last_error,
                                kNetworkGuardWsaAccessDenied,
                                NetworkGuardResult::false_value);
    stubs[static_cast<std::size_t>(NetworkGuardResult::dns_failure)] =
        make_network_guard_stub(remote_wsa_set_last_error,
                                kNetworkGuardDnsFailure,
                                NetworkGuardResult::dns_failure);
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < stubs.size(); ++index) {
        if (stubs[index].empty() || cursor + stubs[index].size() > page_size) {
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            return false;
        }
        SIZE_T written = 0;
        auto* target = reinterpret_cast<std::uint8_t*>(remote) + cursor;
        if (!WriteProcessMemory(process, target, stubs[index].data(),
                                stubs[index].size(), &written) ||
            written != stubs[index].size()) {
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            return false;
        }
        guard.targets[index] = reinterpret_cast<std::uint64_t>(target);
        cursor = (cursor + stubs[index].size() + 15u) & ~std::size_t{15u};
    }
    DWORD old_protect = 0;
    if (!VirtualProtectEx(process, remote, page_size, PAGE_EXECUTE_READ,
                          &old_protect)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    if (!FlushInstructionCache(process, remote, cursor)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    guard.remote_page = reinterpret_cast<std::uint64_t>(remote);
    guard.page_size = page_size;
    return true;
}

inline bool verify_network_guard_page(HANDLE process,
                                      std::uint64_t remote_wsa_set_last_error,
                                      const NetworkGuard& guard) {
    if (!process || !guard.remote_page || guard.page_size != 0x1000)
        return false;
    for (std::size_t index = 0; index < guard.targets.size(); ++index) {
        const auto target = guard.targets[index];
        if (target < guard.remote_page ||
            target >= guard.remote_page + guard.page_size)
            return false;
        const auto expected = make_network_guard_stub(
            remote_wsa_set_last_error,
            index == static_cast<std::size_t>(NetworkGuardResult::dns_failure)
                ? kNetworkGuardDnsFailure
                : kNetworkGuardWsaAccessDenied,
            static_cast<NetworkGuardResult>(index));
        std::vector<std::uint8_t> actual(expected.size());
        SIZE_T read = 0;
        if (expected.empty() ||
            !ReadProcessMemory(process, reinterpret_cast<const void*>(target),
                               actual.data(), actual.size(), &read) ||
            read != actual.size() || actual != expected)
            return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQueryEx(process,
                        reinterpret_cast<const void*>(guard.remote_page),
                        &memory, sizeof(memory)))
        return false;
    return memory.State == MEM_COMMIT && memory.Protect == PAGE_EXECUTE_READ;
}

inline bool seal_network_guard_iat_page(HANDLE process,
                                        std::uint64_t image_base) {
    DWORD old_protect = 0;
    return process &&
           VirtualProtectEx(
               process,
               reinterpret_cast<void*>(image_base +
                                        kNetworkGuardIatPageRva),
               kNetworkGuardIatPageSize, PAGE_READONLY, &old_protect) != FALSE;
}

inline bool verify_network_guard_iat_page(HANDLE process,
                                          std::uint64_t image_base) {
    if (!process)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQueryEx(
            process,
            reinterpret_cast<const void*>(image_base +
                                           kNetworkGuardIatPageRva),
            &memory, sizeof(memory)))
        return false;
    return memory.State == MEM_COMMIT && memory.Protect == PAGE_READONLY &&
           reinterpret_cast<std::uint64_t>(memory.BaseAddress) <=
               image_base + kNetworkGuardIatPageRva &&
           reinterpret_cast<std::uint64_t>(memory.BaseAddress) +
                   memory.RegionSize >=
               image_base + kNetworkGuardIatPageRva +
                   kNetworkGuardIatPageSize;
}

inline bool verify_guarded_import_slots(
    HANDLE process, std::uint64_t image_base,
    const std::vector<GuardedImportSlot>& slots) {
    if (!process || slots.size() != kExpectedGuardedImportCount ||
        !verify_network_guard_iat_page(process, image_base))
        return false;
    for (const auto& slot : slots) {
        if (slot.image_rva < kNetworkGuardIatPageRva ||
            slot.image_rva + sizeof(std::uint64_t) >
                kNetworkGuardIatPageRva + kNetworkGuardIatPageSize)
            return false;
        std::uint64_t actual = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(
                process,
                reinterpret_cast<const void*>(image_base + slot.image_rva),
                &actual, sizeof(actual), &read) ||
            read != sizeof(actual) || actual != slot.target)
            return false;
    }
    return true;
}

}
