#include "kirkware_bone_access_compat.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace kirkware {
namespace {

constexpr std::size_t kRemotePageSize = 0x1000;
constexpr std::size_t kHandlerOffset = 0x100;
constexpr std::size_t kTrampolineOffset = 0x300;
constexpr std::size_t kTagOffset = 0x380;
constexpr std::size_t kPayloadMarkerRva = 0x1E4210;

constexpr std::array<std::uint8_t, 25> kPayloadMarker{
    0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x53, 0x56,
    0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, 0xC0, 0x08, 0x00, 0x00};

constexpr std::array<std::uint8_t, 51> kBoneError{
    0x2A, 0x2A, 0x2A, 0x20, 0x45, 0x52, 0x52, 0x4F, 0x52,
    0x3A, 0x20, 0x42, 0x6F, 0x6E, 0x65, 0x20, 0x61, 0x63,
    0x63, 0x65, 0x73, 0x73, 0x20, 0x6E, 0x6F, 0x74, 0x20,
    0x61, 0x6C, 0x6C, 0x6F, 0x77, 0x65, 0x64, 0x20, 0x28,
    0x65, 0x6E, 0x74, 0x69, 0x74, 0x79, 0x20, 0x25, 0x69,
    0x3A, 0x25, 0x73, 0x29, 0x0A, 0x00};

constexpr std::array<int, 64> kSetupLayout2Pattern{
    0xB8, -1, -1, 0x00, 0x00, 0xE8, -1, -1,
    -1, -1, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05,
    -1, -1, -1, -1, 0x48, 0x33, 0xC4, 0x48,
    0x89, 0x84, 0x24, -1, -1, 0x00, 0x00, 0x48,
    0x89, 0xBC, 0x24, -1, -1, 0x00, 0x00, 0x41,
    0x8B, 0xF9, 0x4C, 0x89, 0xA4, 0x24, -1, -1,
    0x00, 0x00, 0x4C, 0x8B, 0xE2, 0x4C, 0x89, 0xB4,
    0x24, -1, -1, 0x00, 0x00, 0x4C, 0x8B, 0xF1};

constexpr std::array<int, 40> kSetupLayout1Pattern{
    0x40, 0x55, 0x48, 0x8D, 0xAC, 0x24, -1, -1,
    0xFF, 0xFF, 0xB8, -1, -1, 0x00, 0x00, 0xE8,
    -1, -1, -1, -1, 0x48, 0x2B, 0xE0, 0x48,
    0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x33,
    0xC4, 0x48, 0x89, 0x85, -1, -1, 0x00, 0x00};

constexpr std::array<int, 36> kPushPattern{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
    0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x30, 0x49, 0x8B, 0xF0, 0x0F, 0xB6, 0xEA,
    0x44, 0x0F, 0xB6, 0xF1};

constexpr std::array<int, 30> kPopPattern{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xFF, 0x15,
    -1, -1, -1, -1, 0x8B, 0x0D, -1, -1,
    -1, -1, 0x48, 0x8B, 0xD8, 0x3B, 0xC1, 0x74,
    0x2C, 0x45, 0x33, 0xC0, 0x48, 0x8D};

struct ClientImage {
    std::vector<std::uint8_t> bytes;
    std::uint32_t timestamp = 0;
    std::uint32_t image_size = 0;
    std::uint32_t text_rva = 0;
    std::uint32_t text_size = 0;
};

struct Resolution {
    std::uint32_t setup_rva = 0;
    std::uint32_t push_rva = 0;
    std::uint32_t pop_rva = 0;
    std::uint32_t error_rva = 0;
    std::uint32_t layout = 0;
    std::uint32_t patch_length = 0;
};

struct CodeBundle {
    std::vector<std::uint8_t> handler;
    std::vector<std::uint8_t> trampoline;
    std::vector<std::uint8_t> patch;
};

bool AddSafe(std::uint64_t left, std::uint64_t right,
             std::uint64_t& value) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    value = left + right;
    return true;
}

bool ReadExact(HANDLE process, std::uint64_t address, void* output,
               std::size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             output, size, &read) != FALSE && read == size;
}

bool WriteExact(HANDLE process, std::uint64_t address, const void* input,
                std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, reinterpret_cast<void*>(address),
                              input, size, &written) != FALSE &&
           written == size;
}

template <std::size_t Count>
std::vector<std::size_t> FindPattern(const std::uint8_t* bytes,
                                     std::size_t size,
                                     const std::array<int, Count>& pattern) {
    std::vector<std::size_t> matches;
    if (size < Count)
        return matches;
    for (std::size_t offset = 0; offset <= size - Count; ++offset) {
        bool equal = true;
        for (std::size_t index = 0; index < Count; ++index) {
            if (pattern[index] >= 0 &&
                bytes[offset + index] !=
                    static_cast<std::uint8_t>(pattern[index])) {
                equal = false;
                break;
            }
        }
        if (equal)
            matches.push_back(offset);
    }
    return matches;
}

template <std::size_t Count>
std::vector<std::size_t> FindBytes(const std::uint8_t* bytes,
                                   std::size_t size,
                                   const std::array<std::uint8_t, Count>& value) {
    std::vector<std::size_t> matches;
    if (size < Count)
        return matches;
    for (std::size_t offset = 0; offset <= size - Count; ++offset) {
        if (std::memcmp(bytes + offset, value.data(), Count) == 0)
            matches.push_back(offset);
    }
    return matches;
}

bool ReadClientImage(HANDLE process, std::uint64_t base,
                     ClientImage& image,
                     BoneAccessCompatStatus& status) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadExact(process, base, &dos, sizeof(dos))) {
        status = BoneAccessCompatStatus::client_image_unreadable;
        return false;
    }
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x100000) {
        status = BoneAccessCompatStatus::client_image_invalid;
        return false;
    }
    std::uint64_t nt_address = 0;
    if (!AddSafe(base, static_cast<std::uint32_t>(dos.e_lfanew),
                 nt_address)) {
        status = BoneAccessCompatStatus::client_image_invalid;
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadExact(process, nt_address, &nt, sizeof(nt))) {
        status = BoneAccessCompatStatus::client_image_unreadable;
        return false;
    }
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage < 0x10000 ||
        nt.OptionalHeader.SizeOfImage > 0x40000000 ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) {
        status = BoneAccessCompatStatus::client_image_invalid;
        return false;
    }
    const std::uint64_t section_address =
        nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections(
        nt.FileHeader.NumberOfSections);
    if (!ReadExact(process, section_address, sections.data(),
                   sections.size() * sizeof(IMAGE_SECTION_HEADER))) {
        status = BoneAccessCompatStatus::client_image_unreadable;
        return false;
    }
    image.bytes.assign(nt.OptionalHeader.SizeOfImage, 0);
    image.timestamp = nt.FileHeader.TimeDateStamp;
    image.image_size = nt.OptionalHeader.SizeOfImage;
    const std::size_t header_size = nt.OptionalHeader.SizeOfHeaders;
    if (header_size != 0 &&
        !ReadExact(process, base, image.bytes.data(), header_size)) {
        status = BoneAccessCompatStatus::client_image_unreadable;
        return false;
    }
    bool text_read = false;
    for (const auto& section : sections) {
        const std::uint32_t rva = section.VirtualAddress;
        std::uint32_t size = section.Misc.VirtualSize != 0
                                 ? section.Misc.VirtualSize
                                 : section.SizeOfRawData;
        if (rva >= image.image_size)
            continue;
        size = std::min(size, image.image_size - rva);
        if (size == 0)
            continue;
        if (!ReadExact(process, base + rva, image.bytes.data() + rva,
                       size)) {
            if (std::memcmp(section.Name, ".text", 5) == 0) {
                status = BoneAccessCompatStatus::client_image_unreadable;
                return false;
            }
            continue;
        }
        if (std::memcmp(section.Name, ".text", 5) == 0) {
            image.text_rva = rva;
            image.text_size = size;
            text_read = true;
        }
    }
    if (!text_read) {
        status = BoneAccessCompatStatus::client_image_invalid;
        return false;
    }
    return true;
}

bool ResolveClientImage(const ClientImage& image, Resolution& resolution,
                        BoneAccessCompatStatus& status) {
    if (image.text_rva >= image.bytes.size() ||
        image.text_size > image.bytes.size() - image.text_rva) {
        status = BoneAccessCompatStatus::client_image_invalid;
        return false;
    }
    const auto* text = image.bytes.data() + image.text_rva;
    const auto layout2 = FindPattern(text, image.text_size,
                                     kSetupLayout2Pattern);
    const auto layout1 = FindPattern(text, image.text_size,
                                     kSetupLayout1Pattern);
    const auto push = FindPattern(text, image.text_size, kPushPattern);
    const auto pop = FindPattern(text, image.text_size, kPopPattern);
    if (layout1.size() + layout2.size() != 1 || push.size() != 1 ||
        pop.size() != 1) {
        status = BoneAccessCompatStatus::signatures_not_unique;
        return false;
    }
    if (layout2.size() == 1) {
        resolution.setup_rva = image.text_rva +
                               static_cast<std::uint32_t>(layout2.front());
        resolution.layout = 2;
        resolution.patch_length = 13;
    } else {
        resolution.setup_rva = image.text_rva +
                               static_cast<std::uint32_t>(layout1.front());
        resolution.layout = 1;
        resolution.patch_length = 15;
    }
    resolution.push_rva = image.text_rva +
                          static_cast<std::uint32_t>(push.front());
    resolution.pop_rva = image.text_rva +
                         static_cast<std::uint32_t>(pop.front());
    const auto errors = FindBytes(image.bytes.data(), image.bytes.size(),
                                  kBoneError);
    if (errors.size() != 1 ||
        errors.front() > std::numeric_limits<std::uint32_t>::max()) {
        status = BoneAccessCompatStatus::setup_guard_not_verified;
        return false;
    }
    resolution.error_rva = static_cast<std::uint32_t>(errors.front());
    const std::size_t scan_begin = resolution.setup_rva;
    const std::size_t scan_size = std::min<std::size_t>(
        0x400, image.bytes.size() - scan_begin);
    std::size_t xrefs = 0;
    for (std::size_t offset = 0; offset + 7 <= scan_size; ++offset) {
        const auto* instruction = image.bytes.data() + scan_begin + offset;
        if (instruction[0] != 0x48 || instruction[1] != 0x8D ||
            instruction[2] != 0x0D)
            continue;
        std::int32_t displacement = 0;
        std::memcpy(&displacement, instruction + 3, sizeof(displacement));
        const std::int64_t target =
            static_cast<std::int64_t>(resolution.setup_rva) +
            static_cast<std::int64_t>(offset) + 7 + displacement;
        if (target == resolution.error_rva)
            ++xrefs;
    }
    if (xrefs != 1) {
        status = BoneAccessCompatStatus::setup_guard_not_verified;
        return false;
    }
    return true;
}

void Emit32(std::vector<std::uint8_t>& code, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        code.push_back(static_cast<std::uint8_t>(value >> shift));
}

void Emit64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        code.push_back(static_cast<std::uint8_t>(value >> shift));
}

void EmitBytes(std::vector<std::uint8_t>& code,
               std::initializer_list<std::uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
}

void EmitMovRax(std::vector<std::uint8_t>& code, std::uint64_t value) {
    EmitBytes(code, {0x48, 0xB8});
    Emit64(code, value);
}

void EmitMovR8(std::vector<std::uint8_t>& code, std::uint64_t value) {
    EmitBytes(code, {0x49, 0xB8});
    Emit64(code, value);
}

void EmitMovR10(std::vector<std::uint8_t>& code, std::uint64_t value) {
    EmitBytes(code, {0x49, 0xBA});
    Emit64(code, value);
}

void EmitMovR11(std::vector<std::uint8_t>& code, std::uint64_t value) {
    EmitBytes(code, {0x49, 0xBB});
    Emit64(code, value);
}

std::size_t EmitNearBranch(std::vector<std::uint8_t>& code,
                           std::uint8_t condition) {
    EmitBytes(code, {0x0F, condition});
    const std::size_t displacement = code.size();
    Emit32(code, 0);
    return displacement;
}

bool PatchNearBranch(std::vector<std::uint8_t>& code,
                     std::size_t displacement, std::size_t target) {
    if (displacement + 4 > code.size())
        return false;
    const std::int64_t relative =
        static_cast<std::int64_t>(target) -
        static_cast<std::int64_t>(displacement + 4);
    if (relative < std::numeric_limits<std::int32_t>::min() ||
        relative > std::numeric_limits<std::int32_t>::max())
        return false;
    const auto value = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(relative));
    for (unsigned index = 0; index < 4; ++index)
        code[displacement + index] =
            static_cast<std::uint8_t>(value >> (index * 8));
    return true;
}

bool BuildCode(const Resolution& resolution, std::uint64_t client_base,
               std::uint64_t payload_base, std::size_t payload_size,
               std::uint64_t remote_page,
               const std::array<std::uint8_t, 15>& original,
               CodeBundle& bundle) {
    std::uint64_t setup = 0;
    std::uint64_t push = 0;
    std::uint64_t pop = 0;
    std::uint64_t payload_end = 0;
    if (!AddSafe(client_base, resolution.setup_rva, setup) ||
        !AddSafe(client_base, resolution.push_rva, push) ||
        !AddSafe(client_base, resolution.pop_rva, pop) ||
        !AddSafe(payload_base, payload_size, payload_end))
        return false;
    const std::uint64_t handler = remote_page + kHandlerOffset;
    const std::uint64_t trampoline = remote_page + kTrampolineOffset;
    const std::uint64_t tag = remote_page + kTagOffset;
    if (resolution.layout == 2) {
        if (original[0] != 0xB8 || original[5] != 0xE8 ||
            original[10] != 0x48 || original[11] != 0x2B ||
            original[12] != 0xE0)
            return false;
        bundle.trampoline.insert(bundle.trampoline.end(), original.begin(),
                                 original.begin() + 5);
        std::int32_t relative = 0;
        std::memcpy(&relative, original.data() + 6, sizeof(relative));
        const std::uint64_t relative_origin = setup + 10;
        const std::int64_t call_target_signed =
            static_cast<std::int64_t>(relative_origin) + relative;
        if (call_target_signed <= 0)
            return false;
        EmitMovR11(bundle.trampoline,
                   static_cast<std::uint64_t>(call_target_signed));
        EmitBytes(bundle.trampoline, {0x41, 0xFF, 0xD3});
        bundle.trampoline.insert(bundle.trampoline.end(), original.begin() + 10,
                                 original.begin() + 13);
        EmitMovR11(bundle.trampoline, setup + 13);
        EmitBytes(bundle.trampoline, {0x41, 0xFF, 0xE3});
    } else if (resolution.layout == 1) {
        if (original[0] != 0x40 || original[1] != 0x55 ||
            original[2] != 0x48 || original[3] != 0x8D ||
            original[10] != 0xB8)
            return false;
        bundle.trampoline.insert(bundle.trampoline.end(), original.begin(),
                                 original.begin() + 15);
        EmitMovR11(bundle.trampoline, setup + 15);
        EmitBytes(bundle.trampoline, {0x41, 0xFF, 0xE3});
    } else {
        return false;
    }

    EmitBytes(bundle.handler, {0x48, 0x8B, 0x04, 0x24});
    EmitMovR10(bundle.handler, payload_base);
    EmitBytes(bundle.handler, {0x4C, 0x39, 0xD0});
    const std::size_t below = EmitNearBranch(bundle.handler, 0x82);
    EmitMovR10(bundle.handler, payload_end);
    EmitBytes(bundle.handler, {0x4C, 0x39, 0xD0});
    const std::size_t at_or_above = EmitNearBranch(bundle.handler, 0x83);
    EmitBytes(bundle.handler,
              {0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00,
               0x48, 0x89, 0x4C, 0x24, 0x28,
               0x48, 0x89, 0x54, 0x24, 0x30,
               0x4C, 0x89, 0x44, 0x24, 0x38,
               0x4C, 0x89, 0x4C, 0x24, 0x40,
               0x48, 0x8B, 0x84, 0x24, 0xC0, 0x00, 0x00, 0x00,
               0x48, 0x89, 0x44, 0x24, 0x20,
               0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x50,
               0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x60,
               0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x70,
               0xF3, 0x0F, 0x7F, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00,
               0xB9, 0x01, 0x00, 0x00, 0x00,
               0x31, 0xD2});
    EmitMovR8(bundle.handler, tag);
    EmitMovRax(bundle.handler, push);
    EmitBytes(bundle.handler, {0xFF, 0xD0,
                               0x48, 0x8B, 0x4C, 0x24, 0x28,
                               0x48, 0x8B, 0x54, 0x24, 0x30,
                               0x4C, 0x8B, 0x44, 0x24, 0x38,
                               0x4C, 0x8B, 0x4C, 0x24, 0x40,
                               0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x50,
                               0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x60,
                               0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x70,
                               0xF3, 0x0F, 0x6F, 0x9C, 0x24, 0x80,
                               0x00, 0x00, 0x00});
    EmitMovRax(bundle.handler, trampoline);
    EmitBytes(bundle.handler, {0xFF, 0xD0,
                               0x48, 0x89, 0x44, 0x24, 0x48});
    EmitMovRax(bundle.handler, tag);
    EmitBytes(bundle.handler, {0x48, 0x89, 0xC1});
    EmitMovRax(bundle.handler, pop);
    EmitBytes(bundle.handler, {0xFF, 0xD0,
                               0x48, 0x8B, 0x44, 0x24, 0x48,
                               0x48, 0x81, 0xC4, 0x98, 0x00, 0x00, 0x00,
                               0xC3});
    const std::size_t bypass = bundle.handler.size();
    EmitMovR11(bundle.handler, trampoline);
    EmitBytes(bundle.handler, {0x41, 0xFF, 0xE3});
    if (!PatchNearBranch(bundle.handler, below, bypass) ||
        !PatchNearBranch(bundle.handler, at_or_above, bypass))
        return false;

    EmitMovR11(bundle.patch, handler);
    EmitBytes(bundle.patch, {0x41, 0xFF, 0xE3});
    while (bundle.patch.size() < resolution.patch_length)
        bundle.patch.push_back(0x90);
    return bundle.handler.size() <= kTrampolineOffset - kHandlerOffset &&
           bundle.trampoline.size() <= kTagOffset - kTrampolineOffset &&
           bundle.patch.size() == resolution.patch_length;
}

void FillResult(const ClientImage& image, const Resolution& resolution,
                BoneAccessCompatResult& result) {
    result.client_timestamp = image.timestamp;
    result.client_image_size = image.image_size;
    result.setup_bones_rva = resolution.setup_rva;
    result.push_bone_access_rva = resolution.push_rva;
    result.pop_bone_access_rva = resolution.pop_rva;
    result.bone_error_string_rva = resolution.error_rva;
    result.setup_layout = resolution.layout;
    result.patch_length = resolution.patch_length;
}

}

bool InstallKirkwareBoneAccessCompat(
    HANDLE process, std::uint64_t client_base,
    std::uint64_t payload_base, std::size_t payload_size,
    BoneAccessCompatResult& result) {
    result = {};
    if (!process || process == INVALID_HANDLE_VALUE || client_base == 0 ||
        payload_base == 0 || payload_size < kPayloadMarkerRva +
                                             kPayloadMarker.size()) {
        result.status = BoneAccessCompatStatus::invalid_argument;
        result.win32_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    std::array<std::uint8_t, kPayloadMarker.size()> payload_marker{};
    if (!ReadExact(process, payload_base + kPayloadMarkerRva,
                   payload_marker.data(), payload_marker.size())) {
        result.status = BoneAccessCompatStatus::payload_mismatch;
        result.win32_error = GetLastError();
        return false;
    }
    if (payload_marker != kPayloadMarker) {
        result.status = BoneAccessCompatStatus::payload_mismatch;
        result.win32_error = ERROR_BAD_FORMAT;
        return false;
    }
    ClientImage image;
    BoneAccessCompatStatus status = BoneAccessCompatStatus::success;
    if (!ReadClientImage(process, client_base, image, status)) {
        result.status = status;
        result.win32_error = GetLastError();
        return false;
    }
    Resolution resolution;
    if (!ResolveClientImage(image, resolution, status)) {
        result.status = status;
        result.win32_error = ERROR_BAD_FORMAT;
        return false;
    }
    FillResult(image, resolution, result);
    std::array<std::uint8_t, 15> original{};
    std::copy_n(image.bytes.data() + resolution.setup_rva,
                resolution.patch_length, original.data());
    const std::uint64_t setup = client_base + resolution.setup_rva;
    void* allocation = VirtualAllocEx(process, nullptr, kRemotePageSize,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
    if (!allocation) {
        result.status = BoneAccessCompatStatus::remote_allocation_failed;
        result.win32_error = GetLastError();
        return false;
    }
    const std::uint64_t remote_page =
        reinterpret_cast<std::uint64_t>(allocation);
    CodeBundle bundle;
    if (!BuildCode(resolution, client_base, payload_base, payload_size,
                   remote_page, original, bundle)) {
        VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
        result.status = BoneAccessCompatStatus::client_image_invalid;
        result.win32_error = ERROR_BAD_FORMAT;
        return false;
    }
    std::array<std::uint8_t, kRemotePageSize> page{};
    constexpr std::array<std::uint8_t, 8> magic{
        'S', 'S', 'C', 'B', 'O', 'N', 'E', '1'};
    std::copy(magic.begin(), magic.end(), page.begin());
    std::copy(bundle.handler.begin(), bundle.handler.end(),
              page.begin() + kHandlerOffset);
    std::copy(bundle.trampoline.begin(), bundle.trampoline.end(),
              page.begin() + kTrampolineOffset);
    page[kTagOffset] = 's';
    page[kTagOffset + 1] = 's';
    page[kTagOffset + 2] = 'c';
    if (!WriteExact(process, remote_page, page.data(), page.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
        result.status = BoneAccessCompatStatus::remote_write_failed;
        result.win32_error = error;
        return false;
    }
    DWORD page_old = 0;
    if (!VirtualProtectEx(process, allocation, kRemotePageSize,
                          PAGE_EXECUTE_READ, &page_old)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
        result.status = BoneAccessCompatStatus::remote_protect_failed;
        result.win32_error = error;
        return false;
    }
    FlushInstructionCache(process, allocation, kRemotePageSize);
    DWORD target_old = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(setup),
                          resolution.patch_length, PAGE_EXECUTE_READWRITE,
                          &target_old)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
        result.status = BoneAccessCompatStatus::remote_protect_failed;
        result.win32_error = error;
        return false;
    }
    std::array<std::uint8_t, 15> current{};
    bool target_matches = ReadExact(process, setup, current.data(),
                                    resolution.patch_length) &&
                          std::equal(original.begin(),
                                     original.begin() + resolution.patch_length,
                                     current.begin());
    bool write_attempted = false;
    bool patch_written = false;
    bool patch_verified = false;
    DWORD operation_error = ERROR_SUCCESS;
    if (!target_matches) {
        operation_error = GetLastError();
        if (operation_error == ERROR_SUCCESS)
            operation_error = ERROR_REVISION_MISMATCH;
    } else {
        write_attempted = true;
        patch_written = WriteExact(process, setup, bundle.patch.data(),
                                   bundle.patch.size());
        if (!patch_written) {
            operation_error = GetLastError();
        } else {
            std::array<std::uint8_t, 15> verify{};
            patch_verified = ReadExact(process, setup, verify.data(),
                                       bundle.patch.size()) &&
                             std::equal(bundle.patch.begin(),
                                        bundle.patch.end(), verify.begin());
            if (!patch_verified)
                operation_error = GetLastError();
        }
    }
    bool rollback_verified = true;
    if (!patch_verified && write_attempted) {
        rollback_verified = WriteExact(process, setup, original.data(),
                                       resolution.patch_length);
        std::array<std::uint8_t, 15> rollback{};
        rollback_verified = rollback_verified &&
                            ReadExact(process, setup, rollback.data(),
                                      resolution.patch_length) &&
                            std::equal(original.begin(),
                                       original.begin() +
                                           resolution.patch_length,
                                       rollback.begin());
    }
    DWORD ignored = 0;
    bool target_protection_restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(setup),
                         resolution.patch_length, target_old, &ignored) != FALSE;
    const bool protection_failure = !target_protection_restored;
    if (patch_verified && !target_protection_restored) {
        if (operation_error == ERROR_SUCCESS)
            operation_error = GetLastError();
        DWORD rollback_old = 0;
        VirtualProtectEx(process, reinterpret_cast<void*>(setup),
                         resolution.patch_length, PAGE_EXECUTE_READWRITE,
                         &rollback_old);
        rollback_verified = WriteExact(process, setup, original.data(),
                                       resolution.patch_length);
        std::array<std::uint8_t, 15> rollback{};
        rollback_verified = rollback_verified &&
                            ReadExact(process, setup, rollback.data(),
                                      resolution.patch_length) &&
                            std::equal(original.begin(),
                                       original.begin() +
                                           resolution.patch_length,
                                       rollback.begin());
        target_protection_restored =
            VirtualProtectEx(process, reinterpret_cast<void*>(setup),
                             resolution.patch_length, target_old,
                             &ignored) != FALSE;
        patch_verified = false;
    }
    FlushInstructionCache(process, reinterpret_cast<void*>(setup),
                          resolution.patch_length);
    if (!patch_verified || !target_protection_restored) {
        const DWORD final_error = operation_error != ERROR_SUCCESS
                                      ? operation_error
                                      : GetLastError();
        if (!write_attempted || rollback_verified)
            VirtualFreeEx(process, allocation, 0, MEM_RELEASE);
        result.status = protection_failure
                            ? BoneAccessCompatStatus::remote_protect_failed
                            : (!target_matches
                            ? BoneAccessCompatStatus::target_changed
                            : (!patch_written
                                   ? BoneAccessCompatStatus::target_patch_failed
                                   : (!patch_verified
                                          ? BoneAccessCompatStatus::target_verify_failed
                                          : BoneAccessCompatStatus::remote_protect_failed)));
        result.win32_error = final_error;
        return false;
    }
    result.status = BoneAccessCompatStatus::success;
    result.win32_error = ERROR_SUCCESS;
    result.remote_code = remote_page;
    return true;
}

const char* BoneAccessCompatStatusText(BoneAccessCompatStatus status) {
    switch (status) {
    case BoneAccessCompatStatus::success:
        return "success";
    case BoneAccessCompatStatus::invalid_argument:
        return "invalid_argument";
    case BoneAccessCompatStatus::client_image_invalid:
        return "client_image_invalid";
    case BoneAccessCompatStatus::client_image_unreadable:
        return "client_image_unreadable";
    case BoneAccessCompatStatus::signatures_not_unique:
        return "signatures_not_unique";
    case BoneAccessCompatStatus::setup_guard_not_verified:
        return "setup_guard_not_verified";
    case BoneAccessCompatStatus::payload_mismatch:
        return "payload_mismatch";
    case BoneAccessCompatStatus::remote_allocation_failed:
        return "remote_allocation_failed";
    case BoneAccessCompatStatus::remote_write_failed:
        return "remote_write_failed";
    case BoneAccessCompatStatus::remote_protect_failed:
        return "remote_protect_failed";
    case BoneAccessCompatStatus::target_changed:
        return "target_changed";
    case BoneAccessCompatStatus::target_patch_failed:
        return "target_patch_failed";
    case BoneAccessCompatStatus::target_verify_failed:
        return "target_verify_failed";
    }
    return "unknown";
}

}
