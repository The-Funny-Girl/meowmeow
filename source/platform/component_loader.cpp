#include "component_loader.hpp"

#include <dlfcn.h>

#include <cstring>
#include <sstream>

namespace kirkware::platform {
namespace {

bool ValidateStatus(const KirkwareComponentStatus& status, std::string* error)
{
    if (status.abi_version != KIRKWARE_COMPONENT_ABI_VERSION) {
        if (error != nullptr) {
            std::ostringstream stream;
            stream << "component ABI mismatch: expected "
                   << KIRKWARE_COMPONENT_ABI_VERSION << ", got "
                   << status.abi_version;
            *error = stream.str();
        }
        return false;
    }

    if (status.struct_size != sizeof(KirkwareComponentStatus)) {
        if (error != nullptr)
            *error = "component status structure size mismatch";
        return false;
    }

    return true;
}

template <typename Function>
Function Resolve(void* handle, const char* symbol, std::string* error)
{
    dlerror();
    void* address = dlsym(handle, symbol);
    const char* dl_error = dlerror();
    if (dl_error != nullptr) {
        if (error != nullptr)
            *error = std::string("missing component symbol '") + symbol +
                     "': " + dl_error;
        return nullptr;
    }

    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

} // namespace

ComponentSession::~ComponentSession()
{
    Close();
}

bool ComponentSession::Open(const std::filesystem::path& library_path,
                            std::string* error)
{
    Close();

    handle_ = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        if (error != nullptr) {
            const char* dl_error = dlerror();
            *error = dl_error != nullptr ? dl_error : "dlopen failed";
        }
        return false;
    }

    initialize_ = Resolve<KirkwareComponentInitializeFn>(
        handle_, "kirkware_component_initialize", error);
    poll_ = Resolve<KirkwareComponentPollFn>(
        handle_, "kirkware_component_poll", error);
    shutdown_ = Resolve<KirkwareComponentShutdownFn>(
        handle_, "kirkware_component_shutdown", error);

    if (initialize_ == nullptr || poll_ == nullptr || shutdown_ == nullptr) {
        Close();
        return false;
    }

    initial_status_ = {};
    if (initialize_(&initial_status_) != 0) {
        if (error != nullptr)
            *error = "component initialization returned an error";
        Close();
        return false;
    }

    if (!ValidateStatus(initial_status_, error)) {
        Close();
        return false;
    }

    return true;
}

bool ComponentSession::Poll(KirkwareComponentStatus* status,
                            std::string* error) const
{
    if (handle_ == nullptr || poll_ == nullptr) {
        if (error != nullptr)
            *error = "component session is not active";
        return false;
    }
    if (status == nullptr) {
        if (error != nullptr)
            *error = "component status output is null";
        return false;
    }

    *status = {};
    if (poll_(status) != 0) {
        if (error != nullptr)
            *error = "component poll returned an error";
        return false;
    }

    return ValidateStatus(*status, error);
}

void ComponentSession::Close()
{
    if (handle_ == nullptr)
        return;

    if (shutdown_ != nullptr)
        shutdown_();
    dlclose(handle_);

    handle_ = nullptr;
    initialize_ = nullptr;
    poll_ = nullptr;
    shutdown_ = nullptr;
    initial_status_ = {};
}

} // namespace kirkware::platform
