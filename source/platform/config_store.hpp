#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kirkware::platform {

struct LinuxSettings {
    bool keep_workspace = false;
    bool cleanup_stale_workspaces = true;
    unsigned workspace_retention_hours = 24;
};

bool ApplyLinuxSetting(LinuxSettings& settings,
                       std::string_view assignment,
                       std::string* error = nullptr);
bool LoadLinuxSettings(const std::filesystem::path& path,
                       LinuxSettings& settings,
                       bool& existed,
                       std::string* error = nullptr);
bool SaveLinuxSettings(const std::filesystem::path& path,
                       const LinuxSettings& settings,
                       std::string* error = nullptr);

} // namespace kirkware::platform
