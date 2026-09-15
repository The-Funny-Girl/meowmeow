#include "application.hpp"

#include "file_utils.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

namespace kirkware::platform {
namespace {

constexpr unsigned kMinimumRetentionHours = 1;
constexpr unsigned kMaximumRetentionHours = 24 * 30;

void AppendExistsCheck(std::vector<HealthCheck>& checks,
                       std::string name,
                       const std::filesystem::path& path)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        checks.push_back(
            {std::move(name), false,
             "unable to inspect " + path.string() + ": " + ec.message()});
        return;
    }
    checks.push_back({std::move(name), exists, path.string()});
}

} // namespace

bool Application::Initialize(const ApplicationOptions& options,
                             std::string* error)
{
    workspace_.reset();
    logger_.reset();
    paths_ = {};
    settings_ = {};
    settings_path_.clear();

    if (options.workspace_retention_hours &&
        (*options.workspace_retention_hours < kMinimumRetentionHours ||
         *options.workspace_retention_hours > kMaximumRetentionHours)) {
        if (error)
            *error = "workspace_retention_hours must be 1..720";
        return false;
    }

    if (!DiscoverAppPaths(paths_, error))
        return false;
    if (!EnsureAppDirectories(paths_, error))
        return false;

    settings_path_ = paths_.config_root / "linux.conf";
    bool settings_existed = false;
    if (!LoadLinuxSettings(settings_path_, settings_, settings_existed, error))
        return false;
    if (!settings_existed && !SaveLinuxSettings(settings_path_, settings_, error))
        return false;

    if (options.keep_workspace)
        settings_.keep_workspace = *options.keep_workspace;
    if (options.cleanup_stale_workspaces)
        settings_.cleanup_stale_workspaces = *options.cleanup_stale_workspaces;
    if (options.workspace_retention_hours)
        settings_.workspace_retention_hours = *options.workspace_retention_hours;

    logger_ = std::make_unique<Logger>(paths_.logs / "kirkware.log");
    if (!logger_->Initialize(error))
        return false;

    logger_->Write(LogLevel::Info, "Linux application initialization started");

    if (settings_.cleanup_stale_workspaces) {
        std::string cleanup_error;
        const std::size_t removed = CleanupStaleWorkspaces(
            paths_.workspaces,
            std::chrono::hours(settings_.workspace_retention_hours),
            &cleanup_error);
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
            paths_.workspaces, settings_.keep_workspace, error);
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

    AppendExistsCheck(checks, "settings-file", settings_path_);
    if (logger_)
        AppendExistsCheck(checks, "log-file", logger_->path());
    if (workspace_)
        AppendExistsCheck(checks, "workspace", workspace_->path());
    return checks;
}

} // namespace kirkware::platform
