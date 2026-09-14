#include "application.hpp"
#include "config_store.hpp"
#include "file_utils.hpp"
#include "game_integration.hpp"
#include "linux_paths.hpp"
#include "workspace.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Expect(bool condition, const char* description)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
}

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, std::string value) : name_(name)
    {
        if (const char* previous = std::getenv(name); previous) {
            had_previous_ = true;
            previous_ = previous;
        }
        ::setenv(name, value.c_str(), 1);
    }

    ~ScopedEnvironment()
    {
        if (had_previous_)
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

fs::path MakeTestRoot()
{
    const fs::path root = fs::temp_directory_path() /
                          ("kirkware-platform-tests-" +
                           std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

} // namespace

int main()
{
    const fs::path root = MakeTestRoot();
    ScopedEnvironment home("HOME", (root / "home").string());
    ScopedEnvironment config("XDG_CONFIG_HOME", (root / "config").string());
    ScopedEnvironment data("XDG_DATA_HOME", (root / "data").string());
    ScopedEnvironment cache("XDG_CACHE_HOME", (root / "cache").string());
    ScopedEnvironment state("XDG_STATE_HOME", (root / "state").string());
    ScopedEnvironment runtime("XDG_RUNTIME_DIR", (root / "runtime").string());

    fs::create_directories(root / "home");
    fs::create_directories(root / "runtime");

    kirkware::platform::AppPaths paths;
    std::string error;
    {
        ScopedEnvironment bad_config("XDG_CONFIG_HOME", "relative-path");
        kirkware::platform::AppPaths invalid_paths;
        std::string invalid_error;
        Expect(!kirkware::platform::DiscoverAppPaths(
                   invalid_paths, &invalid_error),
               "relative XDG_CONFIG_HOME is rejected");
    }

    Expect(kirkware::platform::DiscoverAppPaths(paths, &error),
           "DiscoverAppPaths succeeds with XDG paths");
    Expect(paths.config_root == root / "config" / "kirkware",
           "config path uses XDG_CONFIG_HOME");
    Expect(paths.data_root == root / "data" / "kirkware",
           "data path uses XDG_DATA_HOME");
    Expect(paths.runtime_root == root / "runtime" / "kirkware",
           "runtime path uses XDG_RUNTIME_DIR");
    Expect(kirkware::platform::EnsureAppDirectories(paths, &error),
           "EnsureAppDirectories creates the Linux tree");
    Expect(fs::is_directory(paths.configs), "configs directory exists");
    Expect(fs::is_directory(paths.autorun_client),
           "autorun/client directory exists");

    const fs::path text_path = paths.config_root / "probe.txt";
    Expect(kirkware::platform::AtomicWriteText(text_path, "hello\n", &error),
           "AtomicWriteText succeeds");
    std::string text;
    Expect(kirkware::platform::ReadTextFile(text_path, 1024, text, &error),
           "ReadTextFile succeeds");
    Expect(text == "hello\n", "ReadTextFile preserves contents");
    std::string too_small;
    Expect(!kirkware::platform::ReadTextFile(text_path, 3, too_small, &error),
           "ReadTextFile enforces maximum size");

    const fs::path settings_path = paths.config_root / "settings-test.conf";
    kirkware::platform::LinuxSettings settings;
    settings.keep_workspace = true;
    settings.cleanup_stale_workspaces = false;
    settings.workspace_retention_hours = 48;
    Expect(kirkware::platform::SaveLinuxSettings(settings_path, settings, &error),
           "SaveLinuxSettings succeeds");
    kirkware::platform::LinuxSettings loaded_settings;
    bool settings_existed = false;
    Expect(kirkware::platform::LoadLinuxSettings(
               settings_path, loaded_settings, settings_existed, &error),
           "LoadLinuxSettings succeeds");
    Expect(settings_existed, "settings file is detected");
    Expect(loaded_settings.keep_workspace, "keep_workspace round trips");
    Expect(!loaded_settings.cleanup_stale_workspaces,
           "cleanup_stale_workspaces round trips");
    Expect(loaded_settings.workspace_retention_hours == 48,
           "workspace_retention_hours round trips");

    const fs::path steam_root = root / "home" / ".local" / "share" / "Steam";
    const fs::path second_library = root / "steam-library";
    fs::create_directories(steam_root / "steamapps");
    fs::create_directories(
        second_library / "steamapps" / "common" / "GarrysMod");
    const std::string library_vdf =
        "\"libraryfolders\"\n{\n    \"0\"\n    {\n        \"path\" \"" +
        second_library.string() + "\"\n    }\n}\n";
    Expect(kirkware::platform::AtomicWriteText(
               steam_root / "steamapps" / "libraryfolders.vdf",
               library_vdf, &error),
           "Steam library fixture can be written");
    Expect(kirkware::platform::AtomicWriteText(
               second_library / "steamapps" / "appmanifest_4000.acf",
               "\"AppState\"\n{\n    \"appid\" \"4000\"\n    \"installdir\" \"GarrysMod\"\n}\n",
               &error),
           "Garry's Mod manifest fixture can be written");
    const auto game = kirkware::platform::DiscoverGarrysMod(root / "home");
    Expect(game.found,
           "Garry's Mod is discovered in a secondary Steam library");
    Expect(game.library_root == second_library,
           "Garry's Mod discovery returns the correct Steam library");
    Expect(game.install_root ==
               second_library / "steamapps" / "common" / "GarrysMod",
           "Garry's Mod discovery returns the install directory");

    const fs::path fake_bin = root / "bin";
    fs::create_directories(fake_bin);
    const fs::path fake_steam = fake_bin / "steam";
    Expect(kirkware::platform::AtomicWriteText(
               fake_steam, "#!/bin/sh\nexit 0\n", &error),
           "fake Steam command can be written");
    fs::permissions(fake_steam,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::owner_exec,
                    fs::perm_options::replace);
    {
        ScopedEnvironment path_override("PATH", fake_bin.string());
        Expect(kirkware::platform::DetectSteamLaunchMethod() ==
                   kirkware::platform::SteamLaunchMethod::SteamCommand,
               "Steam command is preferred when available");
    }

    const fs::path invalid_settings_path =
        paths.config_root / "settings-invalid.conf";
    Expect(kirkware::platform::AtomicWriteText(
               invalid_settings_path, "unknown_key=true\n", &error),
           "invalid settings fixture can be written");
    kirkware::platform::LinuxSettings invalid_settings;
    bool invalid_settings_existed = false;
    Expect(!kirkware::platform::LoadLinuxSettings(
               invalid_settings_path, invalid_settings,
               invalid_settings_existed, &error),
           "unknown Linux setting is rejected");

    fs::path workspace_path;
    {
        auto workspace = kirkware::platform::TemporaryWorkspace::Create(
            paths.workspaces, false, &error);
        Expect(static_cast<bool>(workspace), "workspace creation succeeds");
        if (workspace) {
            workspace_path = workspace->path();
            Expect(fs::is_directory(workspace_path), "workspace directory exists");
        }
    }
    Expect(!workspace_path.empty() && !fs::exists(workspace_path),
           "workspace is removed on destruction");

    fs::path locked_workspace_path;
    {
        auto locked_workspace = kirkware::platform::TemporaryWorkspace::Create(
            paths.workspaces, true, &error);
        Expect(static_cast<bool>(locked_workspace),
               "locked workspace creation succeeds");
        if (locked_workspace) {
            locked_workspace_path = locked_workspace->path();
            std::error_code time_error;
            fs::last_write_time(
                locked_workspace_path,
                fs::file_time_type::clock::now() - std::chrono::hours(48),
                time_error);
            Expect(!time_error, "workspace timestamp can be adjusted for test");
            const std::size_t removed_while_locked =
                kirkware::platform::CleanupStaleWorkspaces(
                    paths.workspaces, std::chrono::hours(24), &error);
            Expect(removed_while_locked == 0,
                   "active locked workspace is not removed as stale");
            Expect(fs::exists(locked_workspace_path),
                   "active locked workspace still exists");
        }
    }
    const std::size_t removed_after_unlock =
        kirkware::platform::CleanupStaleWorkspaces(
            paths.workspaces, std::chrono::hours(24), &error);
    Expect(removed_after_unlock == 1,
           "unlocked stale workspace is removed");
    Expect(!locked_workspace_path.empty() && !fs::exists(locked_workspace_path),
           "stale workspace disappears after unlock");

    kirkware::platform::Application application;
    kirkware::platform::ApplicationOptions options;
    Expect(application.Initialize(options, &error),
           "Application initialization succeeds");
    Expect(fs::exists(application.settings_path()),
           "Application creates linux.conf on first run");
    const auto checks = application.CheckHealth();
    Expect(!checks.empty(), "health checks are produced");
    for (const auto& check : checks)
        Expect(check.ok, check.name.c_str());

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures == 0)
        std::cout << "All Linux platform tests passed\n";
    return failures == 0 ? 0 : 1;
}
