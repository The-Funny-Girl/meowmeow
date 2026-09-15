#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void* g_module_handle = nullptr;
std::string g_module_path;

fs::path control_dir_for(pid_t pid) {
    return fs::path("/tmp") /
           ("kirkware-load-target-" +
            std::to_string(static_cast<unsigned long>(::getuid())) + "-" +
            std::to_string(static_cast<long>(pid)));
}

bool validate_module_path(const std::string& requested,
                          std::string& canonical_path,
                          std::string& error) {
    fs::path input(requested);
    if (!input.is_absolute()) {
        error = "module path must be absolute";
        return false;
    }

    std::error_code ec;
    const fs::path canonical = fs::canonical(input, ec);
    if (ec) {
        error = "cannot resolve module path: " + ec.message();
        return false;
    }

    struct stat info {};
    if (::stat(canonical.c_str(), &info) != 0) {
        error = std::string("cannot stat module: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        error = "module is not a regular file";
        return false;
    }
    if (info.st_uid != ::getuid()) {
        error = "module must be owned by the current user";
        return false;
    }

    canonical_path = canonical.string();
    return true;
}

bool load_module(const std::string& requested, std::string& response) {
    if (g_module_handle != nullptr) {
        response = "ERR a module is already loaded; unload it first\n";
        return false;
    }

    std::string canonical_path;
    std::string validation_error;
    if (!validate_module_path(requested, canonical_path, validation_error)) {
        response = "ERR " + validation_error + "\n";
        return false;
    }

    ::dlerror();
    void* handle = ::dlopen(canonical_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = ::dlerror();
        response = "ERR dlopen failed: " +
                   std::string(error != nullptr ? error : "unknown error") + "\n";
        return false;
    }

    g_module_handle = handle;
    g_module_path = canonical_path;
    response = "OK loaded " + canonical_path + "\n";
    return true;
}

bool unload_module(std::string& response) {
    if (g_module_handle == nullptr) {
        response = "ERR no module is loaded\n";
        return false;
    }

    void* handle = g_module_handle;
    const std::string previous_path = g_module_path;
    g_module_handle = nullptr;
    g_module_path.clear();

    if (::dlclose(handle) != 0) {
        const char* error = ::dlerror();
        response = "ERR dlclose failed: " +
                   std::string(error != nullptr ? error : "unknown error") + "\n";
        return false;
    }

    response = "OK unloaded " + previous_path + "\n";
    return true;
}

std::string status_response() {
    if (g_module_handle == nullptr) {
        return "OK unloaded\n";
    }
    return "OK loaded " + g_module_path + "\n";
}

bool prepare_control_dir(const fs::path& control_dir, std::string& error) {
    struct stat info {};
    if (::lstat(control_dir.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode) || info.st_uid != ::getuid()) {
            error = "existing control path is not a current-user directory";
            return false;
        }
        std::error_code remove_ec;
        fs::remove_all(control_dir, remove_ec);
        if (remove_ec) {
            error = "cannot clear stale control directory: " + remove_ec.message();
            return false;
        }
    } else if (errno != ENOENT) {
        error = std::string("cannot inspect control directory: ") + std::strerror(errno);
        return false;
    }

    const mode_t previous_umask = ::umask(0077);
    std::error_code create_ec;
    const bool created = fs::create_directory(control_dir, create_ec);
    ::umask(previous_umask);
    if (!created || create_ec) {
        error = "cannot create control directory: " + create_ec.message();
        return false;
    }
    if (::chmod(control_dir.c_str(), S_IRWXU) != 0) {
        error = std::string("cannot secure control directory: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool read_request(const fs::path& control_dir,
                  std::string& token,
                  std::string& command,
                  std::string& error) {
    const fs::path request_path = control_dir / "request.txt";
    std::ifstream input(request_path);
    if (!input) {
        error = "request file is missing";
        return false;
    }

    if (!std::getline(input, token) || token.empty()) {
        error = "request token is missing";
        return false;
    }
    if (!std::getline(input, command) || command.empty()) {
        error = "request command is missing";
        return false;
    }

    input.close();
    std::error_code remove_ec;
    fs::remove(request_path, remove_ec);
    return true;
}

bool write_response(const fs::path& control_dir,
                    const std::string& token,
                    const std::string& response,
                    std::string& error) {
    const fs::path temp_path = control_dir / "response.tmp";
    const fs::path response_path = control_dir / "response.txt";

    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output) {
            error = "cannot create response file";
            return false;
        }
        output << token << '\n' << response;
        output.flush();
        if (!output) {
            error = "cannot write response file";
            return false;
        }
    }

    if (::chmod(temp_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = std::string("cannot secure response file: ") + std::strerror(errno);
        return false;
    }

    std::error_code rename_ec;
    fs::rename(temp_path, response_path, rename_ec);
    if (rename_ec) {
        error = "cannot publish response file: " + rename_ec.message();
        return false;
    }
    return true;
}

bool process_request(const fs::path& control_dir, bool& quit_requested) {
    std::string token;
    std::string command;
    std::string request_error;
    if (!read_request(control_dir, token, command, request_error)) {
        return false;
    }

    std::string response;
    if (command == "PING") {
        response = "OK pong\n";
    } else if (command == "STATUS") {
        response = status_response();
    } else if (command == "UNLOAD") {
        unload_module(response);
    } else if (command == "QUIT") {
        response = "OK quitting\n";
        quit_requested = true;
    } else if (command.rfind("LOAD ", 0) == 0 && command.size() > 5) {
        load_module(command.substr(5), response);
    } else {
        response = "ERR unknown command\n";
    }

    std::string response_error;
    if (!write_response(control_dir, token, response, response_error)) {
        std::cerr << "error: " << response_error << '\n';
        return false;
    }
    return true;
}

int self_test(const std::string& module_path) {
    std::string response;
    if (!load_module(module_path, response)) {
        std::cerr << response;
        return 1;
    }
    std::cout << response;
    if (!unload_module(response)) {
        std::cerr << response;
        return 1;
    }
    std::cout << response;
    return 0;
}

void print_help() {
    std::cout
        << "Usage: kirkware-load-target [--self-test /absolute/path/module.so]\n\n"
        << "Runs a cooperative Linux shared-object load target owned by the current user.\n"
        << "The target receives same-user requests through a private control directory\n"
        << "and SIGUSR1, then performs dlopen() itself. No Unix socket is used.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--self-test") {
        return self_test(argv[2]);
    }
    if (argc != 1) {
        print_help();
        return 2;
    }

    const pid_t pid = ::getpid();
    const fs::path control_dir = control_dir_for(pid);

    std::string prepare_error;
    if (!prepare_control_dir(control_dir, prepare_error)) {
        std::cerr << "error: " << prepare_error << '\n';
        return 1;
    }

    sigset_t signal_set {};
    ::sigemptyset(&signal_set);
    ::sigaddset(&signal_set, SIGUSR1);
    ::sigaddset(&signal_set, SIGINT);
    ::sigaddset(&signal_set, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
        std::cerr << "error: sigprocmask: " << std::strerror(errno) << '\n';
        std::error_code cleanup_ec;
        fs::remove_all(control_dir, cleanup_ec);
        return 1;
    }

    std::cout << "Kirkware cooperative load target\n"
              << "  PID:     " << static_cast<long>(pid) << '\n'
              << "  Control: " << control_dir.string() << '\n'
              << "  Signal:  SIGUSR1\n"
              << "Waiting for same-user load requests...\n";
    std::cout.flush();

    bool quit_requested = false;
    while (!quit_requested) {
        int received_signal = 0;
        const int wait_result = ::sigwait(&signal_set, &received_signal);
        if (wait_result != 0) {
            std::cerr << "error: sigwait: " << std::strerror(wait_result) << '\n';
            break;
        }

        if (received_signal == SIGINT || received_signal == SIGTERM) {
            break;
        }
        if (received_signal == SIGUSR1) {
            process_request(control_dir, quit_requested);
        }
    }

    if (g_module_handle != nullptr) {
        std::string ignored;
        unload_module(ignored);
    }

    if (quit_requested) {
        ::usleep(100000);
    }

    std::error_code cleanup_ec;
    fs::remove_all(control_dir, cleanup_ec);
    return 0;
}
