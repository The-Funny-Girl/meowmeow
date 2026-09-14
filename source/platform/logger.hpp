#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace kirkware::platform {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    explicit Logger(std::filesystem::path path);

    bool Initialize(std::string* error = nullptr);
    void Write(LogLevel level, std::string_view message);
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

const char* LogLevelName(LogLevel level) noexcept;

} // namespace kirkware::platform
