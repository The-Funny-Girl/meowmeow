#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace kirkware {

enum class BoneAccessCompatStatus : std::uint32_t {
    success,
    invalid_argument,
    client_image_invalid,
    client_image_unreadable,
    signatures_not_unique,
    setup_guard_not_verified,
    payload_mismatch,
    remote_allocation_failed,
    remote_write_failed,
    remote_protect_failed,
    target_changed,
    target_patch_failed,
    target_verify_failed
};

struct BoneAccessCompatResult {
    BoneAccessCompatStatus status = BoneAccessCompatStatus::invalid_argument;
    DWORD win32_error = ERROR_SUCCESS;
    std::uint32_t client_timestamp = 0;
    std::uint32_t client_image_size = 0;
    std::uint32_t setup_bones_rva = 0;
    std::uint32_t push_bone_access_rva = 0;
    std::uint32_t pop_bone_access_rva = 0;
    std::uint32_t bone_error_string_rva = 0;
    std::uint32_t setup_layout = 0;
    std::uint32_t patch_length = 0;
    std::uint64_t remote_code = 0;
};

bool InstallKirkwareBoneAccessCompat(
    HANDLE process, std::uint64_t client_base,
    std::uint64_t payload_base, std::size_t payload_size,
    BoneAccessCompatResult& result);

const char* BoneAccessCompatStatusText(BoneAccessCompatStatus status);

}
