#include "application.hpp"

#include "file_utils.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

namespace kirkware::platform {

bool Application::Initialize(const ApplicationOptions& options,
                             std::string* error)
{
    if (!DiscoverAppPaths(paths_, error))
        return false;
    if (!EnsureAppDirectories(paths_, error))
        return false;

    logger_ = std::make_unique<Logger>(paths_.logs / "kirkware.log");
    if (!logger_->Initialize(error))
        return false;

    logger_->Write(LogLevel::Info, "Linux application initialization started");

    if (options.cleanup_stale_workspaces) {
        std::string cleanup_error;
        const std::size_t removed = CleanupStaleWorkspaces(
            paths_.workspaces, std::chrono::hours(24), &cleanup_error);
        if (!cleanup_error.empty()) {
            logger_->Write(LogLevel::Warning, cleanup_error);
        } else if (removed != 0) {
            logger_->Write(LogLevel::Info,
                           "removed " + std::to_string(removed) +
                               " stale workspace(s)");
        }
    }

    if (options.create_workspace) {
        workspace_ = TemporaryWorkspace::Create(
            paths_.workspaces, options.keep_workspace, error);
        if (!workspace_) {
            logger_->Write(LogLevel::Error, "workspace creation failed");
            return false;
        }
        logger_->Write(LogLevel::Info,
                       "workspace created at " + workspace_->path().string());
    }

    logger_->Write(LogLevel::Info, "Linux application initialization complete");
    return true;
}

std::vector<HealthCheck> Application::CheckHealth() const
{
    std::vector<HealthCheck> checks;
    for (const auto& [name, path] : NamedPaths(paths_)) {
        std::string error;
        const bool ok = ProbeWritableDirectory(path, &error);
        checks.push_back({name, ok, ok ? path.string() : std::move(error)});
    }
    if (logger_) {
        checks.push_back({"log-file", std::filesystem::exists(logger_->path()),
                          logger_->path().string()});
    }
    if (workspace_) {
        checks.push_back({"workspace", std::filesystem::exists(workspace_->path()),
                          workspace_->path().string()});
    }
    return checks;
}

} // namespace kirkware::platform
