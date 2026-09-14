#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace kirkware::platform {

class TemporaryWorkspace {
public:
    TemporaryWorkspace() = default;
    ~TemporaryWorkspace();

    TemporaryWorkspace(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace& operator=(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace(TemporaryWorkspace&& other) noexcept;
    TemporaryWorkspace& operator=(TemporaryWorkspace&& other) noexcept;

    static std::unique_ptr<TemporaryWorkspace> Create(
        const std::filesystem::path& root,
        bool keep_on_exit,
        std::string* error = nullptr);

    const std::filesystem::path& path() const noexcept { return path_; }
    bool kept() const noexcept { return keep_on_exit_; }
    void Keep(bool keep = true) noexcept { keep_on_exit_ = keep; }
    bool Cleanup(std::string* error = nullptr);

private:
    TemporaryWorkspace(std::filesystem::path path,
                       bool keep_on_exit,
                       int lock_fd);
    void CloseLock() noexcept;

    std::filesystem::path path_;
    bool keep_on_exit_ = false;
    int lock_fd_ = -1;
};

std::size_t CleanupStaleWorkspaces(
    const std::filesystem::path& root,
    std::chrono::hours maximum_age,
    std::string* error = nullptr);

} // namespace kirkware::platform
