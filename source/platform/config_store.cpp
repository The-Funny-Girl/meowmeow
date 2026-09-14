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

bool SetError(std::string* error, std::size_t line, std::string message)
{
    if (error)
        *error = "linux.conf line " + std::to_string(line) + ": " + message;
    return false;
}

} // namespace

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
            return SetError(error, line_number, "expected key=value");
        const std::string_view key = Trim(view.substr(0, equals));
        const std::string_view value = Trim(view.substr(equals + 1));
        if (key == "keep_workspace") {
            if (!ParseBool(value, parsed.keep_workspace))
                return SetError(error, line_number, "invalid keep_workspace value");
        } else if (key == "cleanup_stale_workspaces") {
            if (!ParseBool(value, parsed.cleanup_stale_workspaces))
                return SetError(error, line_number,
                                "invalid cleanup_stale_workspaces value");
        } else if (key == "workspace_retention_hours") {
            if (!ParseUnsigned(value, parsed.workspace_retention_hours) ||
                parsed.workspace_retention_hours < kMinimumRetentionHours ||
                parsed.workspace_retention_hours > kMaximumRetentionHours) {
                return SetError(error, line_number,
                                "workspace_retention_hours must be 1..720");
            }
        } else {
            return SetError(error, line_number,
                            "unknown setting '" + std::string(key) + "'");
        }
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
