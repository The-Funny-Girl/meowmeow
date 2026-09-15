#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxCommandBytes = 8192;

void* g_module_handle = nullptr;
std::string g_module_path;

std::string socket_path_for(pid_t pid) {
    return "/tmp/kirkware-load-target-" + std::to_string(static_cast<unsigned long>(::getuid())) +
           "-" + std::to_string(static_cast<long>(pid)) + ".sock";
}

bool send_all(int fd, std::string_view text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t rc = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

bool receive_line(int fd, std::string& out) {
    out.clear();
    char ch = '\0';
    while (out.size() < kMaxCommandBytes) {
        const ssize_t rc = ::recv(fd, &ch, 1, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return !out.empty();
        }
        if (ch == '\n') {
            return true;
        }
        if (ch != '\r') {
            out.push_back(ch);
        }
    }
    return false;
}

bool same_user_peer(int fd) {
#ifdef SO_PEERCRED
    struct ucred credentials {};
    socklen_t length = static_cast<socklen_t>(sizeof(credentials));
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return false;
    }
    return credentials.uid == ::getuid();
#else
    (void)fd;
    return false;
#endif
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
        response = "ERR dlopen failed: " + std::string(error != nullptr ? error : "unknown error") + "\n";
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
        response = "ERR dlclose failed: " + std::string(error != nullptr ? error : "unknown error") + "\n";
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
        << "The target listens on a private Unix-domain socket and performs dlopen() itself.\n";
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
    const std::string socket_path = socket_path_for(pid);
    sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        std::cerr << "error: generated Unix socket path is too long\n";
        return 1;
    }

    const int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "error: socket: " << std::strerror(errno) << '\n';
        return 1;
    }

    ::unlink(socket_path.c_str());

    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());

    const mode_t previous_umask = ::umask(0077);
    const int bind_result = ::bind(server_fd,
                                   reinterpret_cast<const sockaddr*>(&address),
                                   static_cast<socklen_t>(sizeof(address)));
    ::umask(previous_umask);
    if (bind_result != 0) {
        std::cerr << "error: bind: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }

    if (::chmod(socket_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::cerr << "error: chmod socket: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        ::unlink(socket_path.c_str());
        return 1;
    }

    if (::listen(server_fd, 4) != 0) {
        std::cerr << "error: listen: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        ::unlink(socket_path.c_str());
        return 1;
    }

    std::cout << "Kirkware cooperative load target\n"
              << "  PID:    " << static_cast<long>(pid) << '\n'
              << "  Socket: " << socket_path << '\n'
              << "Waiting for same-user load requests...\n";
    std::cout.flush();

    bool quit_requested = false;
    while (!quit_requested) {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "error: accept: " << std::strerror(errno) << '\n';
            break;
        }

        if (!same_user_peer(client_fd)) {
            send_all(client_fd, "ERR peer UID does not match target UID\n");
            ::close(client_fd);
            continue;
        }

        std::string command;
        if (!receive_line(client_fd, command)) {
            send_all(client_fd, "ERR invalid or oversized command\n");
            ::close(client_fd);
            continue;
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

        send_all(client_fd, response);
        ::close(client_fd);
    }

    if (g_module_handle != nullptr) {
        std::string ignored;
        unload_module(ignored);
    }

    ::close(server_fd);
    ::unlink(socket_path.c_str());
    return 0;
}
