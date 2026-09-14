#include "linux_paths.hpp"

#include "file_utils.hpp"

#include <cstdlib>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kirkware::platform {
namespace {

fs::path EnvironmentPath(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
        return {};
    return fs::path(value);
}

bool RequireAbsoluteOrEmpty(const fs::path& value,
                            const char* variable,
                            std::string* error)
{
    if (value.empty() || value.is_absolute())
        return true;
    if (error)
        *error = std::string(variable) + " must be an absolute path";
    return false;
}

fs::path RootFromXdg(const char* variable,
                     const fs::path& home,
                     const fs::path& fallback)
{
    fs::path value = EnvironmentPath(variable);
    if (!value.empty())
        return value / "kirkware";
    return home / fallback / "kirkware";
}

} // namespace

bool DiscoverAppPaths(AppPaths& paths, std::string* error)
{
    const fs::path home = EnvironmentPath("HOME");
    if (home.empty() || !home.is_absolute()) {
        if (error)
            *error = "HOME is unset or is not an absolute path";
        return false;
    }

    const fs::path xdg_config = EnvironmentPath("XDG_CONFIG_HOME");
    const fs::path xdg_data = EnvironmentPath("XDG_DATA_HOME");
    const fs::path xdg_cache = EnvironmentPath("XDG_CACHE_HOME");
    const fs::path xdg_state = EnvironmentPath("XDG_STATE_HOME");
    const fs::path xdg_runtime = EnvironmentPath("XDG_RUNTIME_DIR");

    if (!RequireAbsoluteOrEmpty(xdg_config, "XDG_CONFIG_HOME", error) ||
        !RequireAbsoluteOrEmpty(xdg_data, "XDG_DATA_HOME", error) ||
        !RequireAbsoluteOrEmpty(xdg_cache, "XDG_CACHE_HOME", error) ||
        !RequireAbsoluteOrEmpty(xdg_state, "XDG_STATE_HOME", error) ||
        !RequireAbsoluteOrEmpty(xdg_runtime, "XDG_RUNTIME_DIR", error)) {
        return false;
    }

    paths.config_root = RootFromXdg("XDG_CONFIG_HOME", home, ".config");
    paths.data_root = RootFromXdg("XDG_DATA_HOME", home, ".local/share");
    paths.cache_root = RootFromXdg("XDG_CACHE_HOME", home, ".cache");
    paths.state_root = RootFromXdg("XDG_STATE_HOME", home, ".local/state");

    if (!xdg_runtime.empty()) {
        paths.runtime_root = xdg_runtime / "kirkware";
    } else {
        std::error_code ec;
        fs::path temp = fs::temp_directory_path(ec);
        if (ec || temp.empty()) {
            if (error)
                *error = "unable to determine a temporary directory";
            return false;
        }
        paths.runtime_root = temp / ("kirkware-" + std::to_string(::getuid()));
    }

    paths.configs = paths.config_root / "configs";
    paths.luas = paths.data_root / "luas";
    paths.autorun_client = paths.data_root / "autorun" / "client";
    paths.autorun_menu = paths.data_root / "autorun" / "menu";
    paths.dumps = paths.state_root / "dumps";
    paths.logs = paths.state_root / "logs";
    paths.workspaces = paths.runtime_root / "workspaces";
    paths.automations = paths.config_root / "automations";
    return true;
}

bool EnsureAppDirectories(const AppPaths& paths, std::string* error)
{
    const fs::path directories[] = {
        paths.config_root,
        paths.data_root,
        paths.cache_root,
        paths.state_root,
        paths.runtime_root,
        paths.configs,
        paths.luas,
        paths.autorun_client,
        paths.autorun_menu,
        paths.dumps,
        paths.logs,
        paths.workspaces,
        paths.automations,
    };

    for (const fs::path& directory : directories) {
        if (!EnsurePrivateDirectory(directory, error))
            return false;
    }
    return true;
}

std::vector<std::pair<std::string, fs::path>> NamedPaths(const AppPaths& paths)
{
    return {
        {"config", paths.config_root},
        {"data", paths.data_root},
        {"cache", paths.cache_root},
        {"state", paths.state_root},
        {"runtime", paths.runtime_root},
        {"configs", paths.configs},
        {"luas", paths.luas},
        {"autorun-client", paths.autorun_client},
        {"autorun-menu", paths.autorun_menu},
        {"dumps", paths.dumps},
        {"logs", paths.logs},
        {"workspaces", paths.workspaces},
        {"automations", paths.automations},
    };
}

} // namespace kirkware::platform
