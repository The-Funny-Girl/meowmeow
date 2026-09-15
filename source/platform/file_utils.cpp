#include "file_utils.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kirkware::platform {
namespace {

std::atomic<unsigned long long> g_temp_counter{0};

void SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}

fs::path TemporarySibling(const fs::path& path)
{
    const auto counter = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
    return path.parent_path() /
           (path.filename().string() + ".tmp." + std::to_string(::getpid()) +
            "." + std::to_string(counter));
}

bool ApplyPrivatePermissions(const fs::path& path,
                             fs::perms permissions,
                             std::string* error)
{
    std::error_code ec;
    fs::permissions(path, permissions, fs::perm_options::replace, ec);
    if (!ec)
        return true;
    SetError(error, "unable to set permissions on " + path.string() + ": " +
                        ec.message());
    return false;
}

} // namespace

bool EnsurePrivateDirectory(const fs::path& path, std::string* error)
{
    if (path.empty()) {
        SetError(error, "empty directory path");
        return false;
    }

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        SetError(error, "unable to create " + path.string() + ": " +
                            ec.message());
        return false;
    }

    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec) {
        SetError(error, "unable to inspect " + path.string() + ": " +
                            ec.message());
        return false;
    }
    if (fs::is_symlink(status)) {
        SetError(error, "refusing symlinked private directory " + path.string());
        return false;
    }
    if (!fs::is_directory(status)) {
        SetError(error, path.string() + " is not a directory");
        return false;
    }
    return ApplyPrivatePermissions(path, fs::perms::owner_all, error);
}

bool AtomicWriteText(const fs::path& path,
                     std::string_view text,
                     std::string* error)
{
    if (path.empty() || path.filename().empty()) {
        SetError(error, "invalid output path");
        return false;
    }
    if (!EnsurePrivateDirectory(path.parent_path(), error))
        return false;

    const fs::path temporary = TemporarySibling(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            SetError(error, "unable to open temporary file " +
                                temporary.string());
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            SetError(error, "unable to write temporary file " +
                                temporary.string());
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }

    if (!ApplyPrivatePermissions(temporary,
                                 fs::perms::owner_read | fs::perms::owner_write,
                                 error)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }

    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        SetError(error, "unable to replace " + path.string() + ": " +
                            ec.message());
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool ReadTextFile(const fs::path& path,
                  std::size_t maximum_size,
                  std::string& text,
                  std::string* error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        SetError(error, "unable to open " + path.string());
        return false;
    }

    std::string result;
    constexpr std::size_t kChunkSize = 8192;
    std::array<char, kChunkSize> buffer{};

    for (;;) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            const std::size_t amount = static_cast<std::size_t>(count);
            if (result.size() > maximum_size ||
                amount > maximum_size - result.size()) {
                SetError(error,
                         path.string() + " exceeds the configured size limit");
                return false;
            }
            result.append(buffer.data(), amount);
        }

        if (stream.eof())
            break;
        if (!stream) {
            SetError(error, "unable to read " + path.string());
            return false;
        }
    }

    text = std::move(result);
    return true;
}

bool ProbeWritableDirectory(const fs::path& path, std::string* error)
{
    if (!EnsurePrivateDirectory(path, error))
        return false;
    const fs::path probe = TemporarySibling(path / ".kirkware-write-probe");
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) {
            SetError(error, "directory is not writable: " + path.string());
            return false;
        }
        stream << "ok\n";
    }
    std::error_code ec;
    fs::remove(probe, ec);
    if (ec) {
        SetError(error, "unable to remove write probe in " + path.string() +
                            ": " + ec.message());
        return false;
    }
    return true;
}

} // namespace kirkware::platform
