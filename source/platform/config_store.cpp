#include "config_store.hpp"

#include "file_utils.hpp"

#include <charconv>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace kirkware::platform {
namespace {

constexpr std::size_t kMaximumSettingsSize = 16 * 1024;
constexpr unsigned kMinimumRetentionHours = 1;
constexpr unsigned kMaximumRetentionHours = 24 * 30;

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

bool ParseBool(std::string_view value, bool& output)
{
    value = Trim(value);
    if (value == "true" || value == "1") {
        output = true;
        return true;
    }
    if (value == "false" || value == "0") {
        output = false;
        return true;
    }
    return false;
}

bool ParseUnsigned(std::string_view value, unsigned& output)
{
    value = Trim(value);
    if (value.empty())
        return false;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, output, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

bool ApplyKeyValue(LinuxSettings& settings,
                   std::string_view key,
                   std::string_view value,
                   std::string* error)
{
    key = Trim(key);
    value = Trim(value);
    if (key == "keep_workspace") {
        if (!ParseBool(value, settings.keep_workspace)) {
            if (error)
                *error = "invalid keep_workspace value";
            return false;
        }
        return true;
    }
    if (key == "cleanup_stale_workspaces") {
        if (!ParseBool(value, settings.cleanup_stale_workspaces)) {
            if (error)
                *error = "invalid cleanup_stale_workspaces value";
            return false;
        }
        return true;
    }
    if (key == "workspace_retention_hours") {
        unsigned hours = 0;
        if (!ParseUnsigned(value, hours) || hours < kMinimumRetentionHours ||
            hours > kMaximumRetentionHours) {
            if (error)
                *error = "workspace_retention_hours must be 1..720";
            return false;
        }
        settings.workspace_retention_hours = hours;
        return true;
    }
    if (error)
        *error = "unknown setting '" + std::string(key) + "'";
    return false;
}

bool SetLineError(std::string* error, std::size_t line, std::string message)
{
    if (error)
        *error = "linux.conf line " + std::to_string(line) + ": " + message;
    return false;
}

} // namespace

bool ApplyLinuxSetting(LinuxSettings& settings,
                       std::string_view assignment,
                       std::string* error)
{
    assignment = Trim(assignment);
    const std::size_t equals = assignment.find('=');
    if (equals == std::string_view::npos) {
        if (error)
            *error = "expected key=value";
        return false;
    }
    const std::string_view key = Trim(assignment.substr(0, equals));
    const std::string_view value = Trim(assignment.substr(equals + 1));
    if (key.empty()) {
        if (error)
            *error = "setting key is empty";
        return false;
    }
    return ApplyKeyValue(settings, key, value, error);
}

bool LoadLinuxSettings(const fs::path& path,
                       LinuxSettings& settings,
                       bool& existed,
                       std::string* error)
{
    std::error_code ec;
    existed = fs::exists(path, ec);
    if (ec) {
        if (error)
            *error = "unable to inspect " + path.string() + ": " + ec.message();
        return false;
    }
    if (!existed) {
        settings = LinuxSettings{};
        return true;
    }

    std::string text;
    if (!ReadTextFile(path, kMaximumSettingsSize, text, error))
        return false;

    LinuxSettings parsed{};
    std::istringstream stream(text);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        std::string_view view = Trim(line);
        if (view.empty() || view.front() == '#')
            continue;
        const std::size_t equals = view.find('=');
        if (equals == std::string_view::npos)
            return SetLineError(error, line_number, "expected key=value");
        const std::string_view key = Trim(view.substr(0, equals));
        const std::string_view value = Trim(view.substr(equals + 1));
        std::string setting_error;
        if (!ApplyKeyValue(parsed, key, value, &setting_error))
            return SetLineError(error, line_number, std::move(setting_error));
    }

    settings = parsed;
    return true;
}

bool SaveLinuxSettings(const fs::path& path,
                       const LinuxSettings& settings,
                       std::string* error)
{
    if (settings.workspace_retention_hours < kMinimumRetentionHours ||
        settings.workspace_retention_hours > kMaximumRetentionHours) {
        if (error)
            *error = "workspace_retention_hours must be 1..720";
        return false;
    }

    std::ostringstream text;
    text << "# Native Linux runtime settings\n"
         << "keep_workspace=" << (settings.keep_workspace ? "true" : "false")
         << '\n'
         << "cleanup_stale_workspaces="
         << (settings.cleanup_stale_workspaces ? "true" : "false") << '\n'
         << "workspace_retention_hours=" << settings.workspace_retention_hours
         << '\n';
    return AtomicWriteText(path, text.str(), error);
}

} // namespace kirkware::platform
