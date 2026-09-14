#include "application.hpp"
#include "game_integration.hpp"
#include "linux_paths.hpp"
#include "kirkware_version.hpp"

#include <iostream>
#include <string_view>

namespace {

void PrintUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  --help             Show this help text\n"
        << "  --version          Print the Linux port version\n"
        << "  --paths            Print resolved Linux/XDG paths\n"
        << "  --settings         Print effective Linux runtime settings\n"
        << "  --game-info        Show Garry's Mod/Steam discovery information\n"
        << "  --launch-game      Launch Garry's Mod through Steam\n"
        << "  --check            Run filesystem/runtime health checks\n"
        << "  --no-workspace     Do not create a temporary workspace\n"
        << "  --keep-workspace   Keep the temporary workspace after exit\n";
}

void PrintSettings(const kirkware::platform::Application& application)
{
    const auto& settings = application.settings();
    std::cout << "settings-file=" << application.settings_path() << '\n'
              << "keep_workspace="
              << (settings.keep_workspace ? "true" : "false") << '\n'
              << "cleanup_stale_workspaces="
              << (settings.cleanup_stale_workspaces ? "true" : "false") << '\n'
              << "workspace_retention_hours="
              << settings.workspace_retention_hours << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    bool show_paths = false;
    bool show_settings = false;
    bool run_checks = false;
    bool show_game_info = false;
    bool launch_game = false;
    kirkware::platform::ApplicationOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argument == "--version") {
            std::cout << "kirkware " << kirkware::build::kVersionLabel << '\n';
            return 0;
        }
        if (argument == "--paths") {
            show_paths = true;
            continue;
        }
        if (argument == "--settings") {
            show_settings = true;
            continue;
        }
        if (argument == "--game-info") {
            show_game_info = true;
            continue;
        }
        if (argument == "--launch-game") {
            launch_game = true;
            continue;
        }
        if (argument == "--check") {
            run_checks = true;
            continue;
        }
        if (argument == "--no-workspace") {
            options.create_workspace = false;
            continue;
        }
        if (argument == "--keep-workspace") {
            options.keep_workspace = true;
            continue;
        }
        std::cerr << "Unknown option: " << argument << "\n\n";
        PrintUsage(argv[0]);
        return 2;
    }

    kirkware::platform::Application application;
    std::string error;
    if (!application.Initialize(options, &error)) {
        std::cerr << "Initialization failed: " << error << '\n';
        return 1;
    }

    if (show_paths) {
        for (const auto& [name, path] :
             kirkware::platform::NamedPaths(application.paths())) {
            std::cout << name << '=' << path << '\n';
        }
    }

    if (show_settings)
        PrintSettings(application);

    if (show_game_info) {
        const auto game = kirkware::platform::DiscoverGarrysMod();
        const auto launcher = kirkware::platform::DetectSteamLaunchMethod();
        std::cout << "game-found=" << (game.found ? "true" : "false") << '\n'
                  << "launcher="
                  << kirkware::platform::SteamLaunchMethodName(launcher) << '\n'
                  << "game-detail=" << game.detail << '\n';
        if (game.found) {
            std::cout << "steam-root=" << game.steam_root << '\n'
                      << "library-root=" << game.library_root << '\n'
                      << "manifest=" << game.manifest_path << '\n'
                      << "install-root=" << game.install_root << '\n';
        }
    }

    if (launch_game) {
        std::string launch_error;
        if (!kirkware::platform::LaunchGarrysMod(&launch_error)) {
            std::cerr << "Unable to launch Garry's Mod: " << launch_error << '\n';
            return 1;
        }
        std::cout << "Garry's Mod launch request sent through "
                  << kirkware::platform::SteamLaunchMethodName(
                         kirkware::platform::DetectSteamLaunchMethod())
                  << '\n';
    }

    if (run_checks) {
        bool all_ok = true;
        for (const auto& check : application.CheckHealth()) {
            std::cout << (check.ok ? "PASS" : "FAIL") << "  " << check.name
                      << "  " << check.detail << '\n';
            all_ok = all_ok && check.ok;
        }
        return all_ok ? 0 : 1;
    }

    if (!show_paths && !show_settings && !show_game_info && !launch_game) {
        std::cout << "kirkware Linux runtime initialized\n";
        std::cout << "config: " << application.paths().config_root << '\n';
        if (application.workspace())
            std::cout << "workspace: " << application.workspace()->path() << '\n';
        std::cout << "log: " << application.paths().logs / "kirkware.log" << '\n';
    }
    return 0;
}
