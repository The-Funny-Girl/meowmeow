#include "game_integration.hpp"

#include "file_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace kirkware::platform {
namespace {

constexpr std::string_view kGarrysModAppId = "4000";
constexpr std::size_t kMaximumVdfSize = 2 * 1024 * 1024;

std::vector<std::string> ParseQuotedTokens(std::string_view line)
{
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < line.size()) {
        const std::size_t open = line.find('"', offset);
        if (open == std::string_view::npos)
            break;
        std::string token;
        bool escaped = false;
        std::size_t index = open + 1;
        for (; index < line.size(); ++index) {
            const char value = line[index];
            if (escaped) {
                token.push_back(value);
                escaped = false;
                continue;
            }
            if (value == '\\') {
                escaped = true;
                continue;
            }
            if (value == '"')
                break;
            token.push_back(value);
        }
        if (index == line.size())
            break;
        result.push_back(std::move(token));
        offset = index + 1;
    }
    return result;
}

std::string VdfValue(const std::string& text, std::string_view wanted_key)
{
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t end = text.find('\n', offset);
        const std::string_view line(
            text.data() + offset,
            (end == std::string::npos ? text.size() : end) - offset);
        const auto tokens = ParseQuotedTokens(line);
        if (tokens.size() >= 2 && tokens[0] == wanted_key)
            return tokens[1];
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
    return {};
}

std::vector<fs::path> CandidateSteamRoots(const fs::path& home)
{
    return {
        home / ".local" / "share" / "Steam",
        home / ".steam" / "steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" /
            "share" / "Steam",
    };
}

void AddUniquePath(std::vector<fs::path>& output,
                   std::set<std::string>& witnessed,
                   const fs::path& path)
{
    if (path.empty())
        return;
    const fs::path normalized = path.lexically_normal();
    if (witnessed.insert(normalized.string()).second)
        output.push_back(normalized);
}

std::vector<fs::path> SteamLibraries(const fs::path& steam_root)
{
    std::vector<fs::path> libraries;
    std::set<std::string> witnessed;
    AddUniquePath(libraries, witnessed, steam_root);

    const fs::path file = steam_root / "steamapps" / "libraryfolders.vdf";
    std::string text;
    if (!ReadTextFile(file, kMaximumVdfSize, text, nullptr))
        return libraries;

    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t end = text.find('\n', offset);
        const std::string_view line(
            text.data() + offset,
            (end == std::string::npos ? text.size() : end) - offset);
        const auto tokens = ParseQuotedTokens(line);
        if (tokens.size() >= 2 && tokens[0] == "path")
            AddUniquePath(libraries, witnessed, fs::path(tokens[1]));
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
    return libraries;
}

bool IsExecutable(const fs::path& path)
{
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

fs::path FindOnPath(std::string_view name)
{
    const char* raw_path = std::getenv("PATH");
    if (!raw_path || !*raw_path)
        return {};
    const std::string path(raw_path);
    std::size_t offset = 0;
    while (offset <= path.size()) {
        const std::size_t end = path.find(':', offset);
        const std::string_view component(
            path.data() + offset,
            (end == std::string::npos ? path.size() : end) - offset);
        if (!component.empty()) {
            const fs::path candidate = fs::path(component) / name;
            if (IsExecutable(candidate))
                return candidate;
        }
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
    return {};
}

bool SpawnDetached(const char* program,
                   const std::vector<std::string>& arguments,
                   std::string* error)
{
    const pid_t child = ::fork();
    if (child < 0) {
        if (error)
            *error = "unable to fork launcher process";
        return false;
    }
    if (child == 0) {
        if (::setsid() < 0)
            _exit(126);
        const pid_t grandchild = ::fork();
        if (grandchild < 0)
            _exit(126);
        if (grandchild > 0)
            _exit(0);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(program));
        for (const std::string& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execvp(program, argv.data());
        _exit(127);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        if (error)
            *error = "unable to start detached launcher process";
        return false;
    }
    return true;
}

} // namespace

GarrysModInstallation DiscoverGarrysMod(const fs::path& home)
{
    GarrysModInstallation result;
    if (home.empty() || !home.is_absolute()) {
        result.detail = "HOME is unset or is not an absolute path";
        return result;
    }

    std::error_code ec;
    for (const fs::path& steam_root : CandidateSteamRoots(home)) {
        if (!fs::is_directory(steam_root, ec)) {
            ec.clear();
            continue;
        }
        for (const fs::path& library : SteamLibraries(steam_root)) {
            const fs::path manifest =
                library / "steamapps" /
                ("appmanifest_" + std::string(kGarrysModAppId) + ".acf");
            if (!fs::is_regular_file(manifest, ec)) {
                ec.clear();
                continue;
            }

            std::string manifest_text;
            std::string install_dir = "GarrysMod";
            if (ReadTextFile(manifest, kMaximumVdfSize, manifest_text, nullptr)) {
                const std::string parsed = VdfValue(manifest_text, "installdir");
                if (!parsed.empty())
                    install_dir = parsed;
            }

            const fs::path install_root =
                library / "steamapps" / "common" / install_dir;
            if (!fs::is_directory(install_root, ec)) {
                ec.clear();
                result.detail = "Garry's Mod manifest exists but install directory is missing";
                continue;
            }

            result.found = true;
            result.steam_root = steam_root;
            result.library_root = library;
            result.manifest_path = manifest;
            result.install_root = install_root;
            result.detail = "Garry's Mod installation found";
            return result;
        }
    }

    if (result.detail.empty())
        result.detail = "Garry's Mod was not found in known Steam libraries";
    return result;
}

GarrysModInstallation DiscoverGarrysMod()
{
    const char* home = std::getenv("HOME");
    return DiscoverGarrysMod(home && *home ? fs::path(home) : fs::path{});
}

SteamLaunchMethod DetectSteamLaunchMethod()
{
    if (!FindOnPath("steam").empty())
        return SteamLaunchMethod::SteamCommand;
    if (!FindOnPath("xdg-open").empty())
        return SteamLaunchMethod::XdgOpen;
    return SteamLaunchMethod::Unavailable;
}

const char* SteamLaunchMethodName(SteamLaunchMethod method) noexcept
{
    switch (method) {
    case SteamLaunchMethod::SteamCommand:
        return "steam";
    case SteamLaunchMethod::XdgOpen:
        return "xdg-open";
    case SteamLaunchMethod::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

bool LaunchGarrysMod(std::string* error)
{
    switch (DetectSteamLaunchMethod()) {
    case SteamLaunchMethod::SteamCommand:
        return SpawnDetached("steam", {"-applaunch", "4000"}, error);
    case SteamLaunchMethod::XdgOpen:
        return SpawnDetached("xdg-open", {"steam://rungameid/4000"}, error);
    case SteamLaunchMethod::Unavailable:
        if (error)
            *error = "neither steam nor xdg-open is available in PATH";
        return false;
    }
    if (error)
        *error = "no Steam launch method is available";
    return false;
}

} // namespace kirkware::platform
