#pragma once

#include <filesystem>
#include <string>

namespace kirkware::platform {

struct GarrysModInstallation {
    bool found = false;
    std::filesystem::path steam_root;
    std::filesystem::path library_root;
    std::filesystem::path manifest_path;
    std::filesystem::path install_root;
    std::string detail;
};

enum class SteamLaunchMethod {
    SteamCommand,
    XdgOpen,
    Unavailable,
};

GarrysModInstallation DiscoverGarrysMod();
GarrysModInstallation DiscoverGarrysMod(const std::filesystem::path& home);
SteamLaunchMethod DetectSteamLaunchMethod();
const char* SteamLaunchMethodName(SteamLaunchMethod method) noexcept;
bool LaunchGarrysMod(std::string* error = nullptr);

} // namespace kirkware::platform
