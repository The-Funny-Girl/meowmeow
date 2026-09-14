#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace kirkware::platform {

struct AppPaths {
    std::filesystem::path config_root;
    std::filesystem::path data_root;
    std::filesystem::path cache_root;
    std::filesystem::path state_root;
    std::filesystem::path runtime_root;

    std::filesystem::path configs;
    std::filesystem::path luas;
    std::filesystem::path autorun_client;
    std::filesystem::path autorun_menu;
    std::filesystem::path dumps;
    std::filesystem::path logs;
    std::filesystem::path workspaces;
    std::filesystem::path automations;
};

bool DiscoverAppPaths(AppPaths& paths, std::string* error = nullptr);
bool EnsureAppDirectories(const AppPaths& paths, std::string* error = nullptr);
std::vector<std::pair<std::string, std::filesystem::path>> NamedPaths(
    const AppPaths& paths);

} // namespace kirkware::platform
