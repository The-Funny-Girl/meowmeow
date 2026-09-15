#include "workspace.hpp"

#include "file_utils.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kirkware::platform {
namespace {

std::atomic<unsigned long long> g_workspace_counter{0};
constexpr const char* kLockFileName = ".workspace.lock";

enum class WorkspaceActivity {
    Inactive,
    Active,
    Unknown,
};

fs::path CandidatePath(const fs::path& root)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto counter =
        g_workspace_counter.fetch_add(1, std::memory_order_relaxed);
    return root / ("session-" + std::to_string(::getpid()) + "-" +
                   std::to_string(now) + "-" + std::to_string(counter));
}

int AcquireWorkspaceLock(const fs::path& directory, std::string* error)
{
    const fs::path lock_path = directory / kLockFileName;
    const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error)
            *error = "unable to open workspace lock " + lock_path.string() +
                     ": " + std::strerror(errno);
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
        return fd;
    if (error)
        *error = "unable to lock workspace " + directory.string() + ": " +
                 std::strerror(errno);
    ::close(fd);
    return -1;
}

WorkspaceActivity GetWorkspaceActivity(const fs::path& directory)
{
    const fs::path lock_path = directory / kLockFileName;
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? WorkspaceActivity::Inactive
                               : WorkspaceActivity::Unknown;

    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return WorkspaceActivity::Inactive;
    }

    const int lock_error = errno;
    ::close(fd);
    if (lock_error == EWOULDBLOCK || lock_error == EAGAIN)
        return WorkspaceActivity::Active;
    return WorkspaceActivity::Unknown;
}

} // namespace

TemporaryWorkspace::TemporaryWorkspace(fs::path path,
                                       bool keep_on_exit,
                                       int lock_fd)
    : path_(std::move(path)), keep_on_exit_(keep_on_exit), lock_fd_(lock_fd)
{
}

TemporaryWorkspace::~TemporaryWorkspace()
{
    if (!keep_on_exit_)
        Cleanup(nullptr);
    CloseLock();
}

TemporaryWorkspace::TemporaryWorkspace(TemporaryWorkspace&& other) noexcept
    : path_(std::move(other.path_)),
      keep_on_exit_(other.keep_on_exit_),
      lock_fd_(other.lock_fd_)
{
    other.path_.clear();
    other.keep_on_exit_ = true;
    other.lock_fd_ = -1;
}

TemporaryWorkspace& TemporaryWorkspace::operator=(
    TemporaryWorkspace&& other) noexcept
{
    if (this == &other)
        return *this;
    if (!keep_on_exit_)
        Cleanup(nullptr);
    CloseLock();
    path_ = std::move(other.path_);
    keep_on_exit_ = other.keep_on_exit_;
    lock_fd_ = other.lock_fd_;
    other.path_.clear();
    other.keep_on_exit_ = true;
    other.lock_fd_ = -1;
    return *this;
}

void TemporaryWorkspace::CloseLock() noexcept
{
    if (lock_fd_ < 0)
        return;
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
    lock_fd_ = -1;
}

std::unique_ptr<TemporaryWorkspace> TemporaryWorkspace::Create(
    const fs::path& root,
    bool keep_on_exit,
    std::string* error)
{
    if (!EnsurePrivateDirectory(root, error))
        return nullptr;

    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        const fs::path candidate = CandidatePath(root);
        std::error_code ec;
        if (!fs::create_directory(candidate, ec)) {
            if (!ec)
                continue;
            if (error)
                *error = "unable to create workspace in " + root.string() +
                         ": " + ec.message();
            return nullptr;
        }
        fs::permissions(candidate, fs::perms::owner_all,
                        fs::perm_options::replace, ec);
        if (ec) {
            fs::remove_all(candidate, ec);
            if (error)
                *error = "unable to set workspace permissions";
            return nullptr;
        }
        const int lock_fd = AcquireWorkspaceLock(candidate, error);
        if (lock_fd < 0) {
            fs::remove_all(candidate, ec);
            return nullptr;
        }
        return std::unique_ptr<TemporaryWorkspace>(
            new TemporaryWorkspace(candidate, keep_on_exit, lock_fd));
    }

    if (error)
        *error = "unable to allocate a unique workspace";
    return nullptr;
}

bool TemporaryWorkspace::Cleanup(std::string* error)
{
    if (path_.empty())
        return true;
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (ec) {
        if (error)
            *error = "unable to remove workspace " + path_.string() + ": " +
                     ec.message();
        return false;
    }
    path_.clear();
    CloseLock();
    return true;
}

std::size_t CleanupStaleWorkspaces(const fs::path& root,
                                   std::chrono::hours maximum_age,
                                   std::string* error)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
        return 0;
    if (ec) {
        if (error)
            *error = "unable to inspect workspace root: " + ec.message();
        return 0;
    }

    const auto cutoff = fs::file_time_type::clock::now() - maximum_age;
    std::size_t removed = 0;
    fs::directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && iterator != end) {
        const fs::directory_entry entry = *iterator;
        iterator.increment(ec);

        std::error_code item_ec;
        if (!entry.is_directory(item_ec) || item_ec)
            continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("session-", 0) != 0)
            continue;
        const auto modified = entry.last_write_time(item_ec);
        if (item_ec || modified >= cutoff)
            continue;
        if (GetWorkspaceActivity(entry.path()) != WorkspaceActivity::Inactive)
            continue;
        fs::remove_all(entry.path(), item_ec);
        if (!item_ec)
            ++removed;
    }
    if (ec && error)
        *error = "unable to enumerate workspaces: " + ec.message();
    return removed;
}

} // namespace kirkware::platform
