#include "logger.hpp"

#include "file_utils.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <utility>

namespace kirkware::platform {

Logger::Logger(std::filesystem::path path) : path_(std::move(path)) {}

bool Logger::Initialize(std::string* error)
{
    if (!EnsurePrivateDirectory(path_.parent_path(), error))
        return false;
    std::ofstream stream(path_, std::ios::app);
    if (!stream) {
        if (error)
            *error = "unable to open log file " + path_.string();
        return false;
    }
    std::error_code ec;
    std::filesystem::permissions(
        path_,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        if (error)
            *error = "unable to set log file permissions: " + ec.message();
        return false;
    }
    return true;
}

void Logger::Write(LogLevel level, std::string_view message)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t when = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&when, &utc);

    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream stream(path_, std::ios::app);
    if (!stream)
        return;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << " ["
           << LogLevelName(level) << "] " << message << '\n';
}

const char* LogLevelName(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

} // namespace kirkware::platform
