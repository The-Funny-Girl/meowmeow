#pragma once

#include "linux_paths.hpp"
#include "logger.hpp"
#include "workspace.hpp"

#include <memory>
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
    bool keep_workspace = false;
    bool cleanup_stale_workspaces = true;
};

class Application {
public:
    bool Initialize(const ApplicationOptions& options,
                    std::string* error = nullptr);

    const AppPaths& paths() const noexcept { return paths_; }
    const TemporaryWorkspace* workspace() const noexcept {
        return workspace_.get();
    }
    Logger* logger() noexcept { return logger_.get(); }
    std::vector<HealthCheck> CheckHealth() const;

private:
    AppPaths paths_{};
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<TemporaryWorkspace> workspace_;
};

} // namespace kirkware::platform
