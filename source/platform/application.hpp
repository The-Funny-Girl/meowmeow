#pragma once

#include "config_store.hpp"
#include "linux_paths.hpp"
#include "logger.hpp"
#include "workspace.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kirkware::platform {

struct HealthCheck {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct ApplicationOptions {
    bool create_workspace = true;
    std::optional<bool> keep_workspace;
    std::optional<bool> cleanup_stale_workspaces;
    std::optional<unsigned> workspace_retention_hours;
};

class Application {
public:
    bool Initialize(const ApplicationOptions& options,
                    std::string* error = nullptr);

    const AppPaths& paths() const noexcept { return paths_; }
    const LinuxSettings& settings() const noexcept { return settings_; }
    const std::filesystem::path& settings_path() const noexcept {
        return settings_path_;
    }
    const TemporaryWorkspace* workspace() const noexcept {
        return workspace_.get();
    }
    Logger* logger() noexcept { return logger_.get(); }
    std::vector<HealthCheck> CheckHealth() const;

private:
    AppPaths paths_{};
    LinuxSettings settings_{};
    std::filesystem::path settings_path_;
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<TemporaryWorkspace> workspace_;
};

} // namespace kirkware::platform
