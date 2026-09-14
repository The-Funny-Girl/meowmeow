#pragma once

#include <cstddef>
#include <cstdint>

namespace kirkware {

struct ImportDescriptor {
    std::uint32_t original_first_thunk;
    std::uint32_t timestamp;
    std::uint32_t forwarder_chain;
    std::uint32_t name;
    std::uint32_t first_thunk;
};

inline bool in_image(std::size_t image_size, std::uint64_t rva, std::size_t size) {
    return rva <= image_size && size <= image_size - static_cast<std::size_t>(rva);
}

enum class AuxiliaryKind {
    export_name,
    module_base,
    module_rva
};

struct AuxiliaryPatch {
    std::uint64_t image_rva;
    AuxiliaryKind kind;
    const char *module;
    const char *symbol;
    std::uint64_t module_rva;
};

inline constexpr AuxiliaryPatch auxiliary_patches[] = {
    {0x7E5130, AuxiliaryKind::export_name, "kernel32.dll", "GetSystemTimePreciseAsFileTime", 0},
    {0x7E5138, AuxiliaryKind::export_name, "kernel32.dll", "GetTempPath2W", 0},
    {0x7E5828, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E5830, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E5840, AuxiliaryKind::export_name, "kernelbase.dll", "FlsAlloc", 0},
    {0x7E5850, AuxiliaryKind::export_name, "kernelbase.dll", "FlsGetValue", 0},
    {0x7E5858, AuxiliaryKind::export_name, "kernelbase.dll", "FlsSetValue", 0},
    {0x7E5860, AuxiliaryKind::export_name, "kernelbase.dll", "InitializeCriticalSectionEx", 0},
    {0x7E60E0, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E60E8, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E6108, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E6110, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E6120, AuxiliaryKind::module_base, "kernelbase.dll", nullptr, 0},
    {0x7E6170, AuxiliaryKind::module_base, "kernel32.dll", nullptr, 0},
    {0xB812F8, AuxiliaryKind::export_name, "client.dll", "CreateInterface", 0},
    {0xB81358, AuxiliaryKind::export_name, "vstdlib.dll", "CreateInterface", 0},
    {0xB81368, AuxiliaryKind::export_name, "materialsystem.dll", "CreateInterface", 0},
    {0xB81380, AuxiliaryKind::export_name, "inputsystem.dll", "CreateInterface", 0},
    {0xB81390, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB813B8, AuxiliaryKind::export_name, "client.dll", "CreateInterface", 0},
    {0xB813C0, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB813D0, AuxiliaryKind::export_name, "studiorender.dll", "CreateInterface", 0},
    {0xB81408, AuxiliaryKind::export_name, "lua_shared.dll", "CreateInterface", 0},
    {0xB81418, AuxiliaryKind::export_name, "vgui2.dll", "CreateInterface", 0},
    {0xB81428, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB81440, AuxiliaryKind::export_name, "client.dll", "CreateInterface", 0},
    {0xB81470, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB81498, AuxiliaryKind::export_name, "client.dll", "CreateInterface", 0},
    {0xB814B8, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB814D0, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB814E8, AuxiliaryKind::export_name, "vguimatsurface.dll", "CreateInterface", 0},
    {0xB81500, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xB81518, AuxiliaryKind::export_name, "engine.dll", "CreateInterface", 0},
    {0xBB8000, AuxiliaryKind::export_name, "kernel32.dll", "AreFileApisANSI", 0},
    {0xBB8008, AuxiliaryKind::export_name, "kernelbase.dll", "CompareStringEx", 0},
    {0xBB8010, AuxiliaryKind::export_name, "kernelbase.dll", "EnumSystemLocalesEx", 0},
    {0xBB8028, AuxiliaryKind::export_name, "kernelbase.dll", "GetDateFormatEx", 0},
    {0xBB8050, AuxiliaryKind::export_name, "kernelbase.dll", "GetLocaleInfoEx", 0},
    {0xBB8068, AuxiliaryKind::export_name, "kernelbase.dll", "GetTimeFormatEx", 0},
    {0xBB8070, AuxiliaryKind::export_name, "kernelbase.dll", "GetUserDefaultLocaleName", 0},
    {0xBB8088, AuxiliaryKind::export_name, "kernelbase.dll", "IsValidLocaleName", 0},
    {0xBB8090, AuxiliaryKind::export_name, "kernelbase.dll", "LCMapStringEx", 0},
    {0xBB8098, AuxiliaryKind::export_name, "kernelbase.dll", "LCIDToLocaleName", 0},
    {0xBB80A0, AuxiliaryKind::export_name, "kernelbase.dll", "LocaleNameToLCID", 0},
    {0xBB80F8, AuxiliaryKind::export_name, "cryptbase.dll", "SystemFunction036", 0}
};

}
