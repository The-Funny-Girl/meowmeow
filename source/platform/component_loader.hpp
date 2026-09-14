#pragma once

#include "component_api.h"

#include <filesystem>
#include <string>

namespace kirkware::platform {

class ComponentSession {
public:
    ComponentSession() = default;
    ~ComponentSession();

    ComponentSession(const ComponentSession&) = delete;
    ComponentSession& operator=(const ComponentSession&) = delete;

    bool Open(const std::filesystem::path& library_path, std::string* error);
    bool Poll(KirkwareComponentStatus* status, std::string* error) const;
    void Close();

    bool active() const noexcept { return handle_ != nullptr; }
    const KirkwareComponentStatus& initial_status() const noexcept
    {
        return initial_status_;
    }

private:
    void* handle_ = nullptr;
    KirkwareComponentInitializeFn initialize_ = nullptr;
    KirkwareComponentPollFn poll_ = nullptr;
    KirkwareComponentShutdownFn shutdown_ = nullptr;
    KirkwareComponentStatus initial_status_{};
};

} // namespace kirkware::platform
