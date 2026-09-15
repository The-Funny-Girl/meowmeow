#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Target {
    pid_t pid {};
    fs::path socket_path;
};

std::string target_prefix() {
    return "kirkware-load-target-" +
           std::to_string(static_cast<unsigned long>(::getuid())) + "-";
}

std::vector<Target> discover_targets() {
    std::vector<Target> targets;
    const std::string prefix = target_prefix();
    constexpr std::string_view suffix = ".sock";
    const uid_t current_uid = ::getuid();

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator("/tmp", ec)) {
        if (ec) {
            break;
        }

        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
            continue;
        }

        struct stat info {};
        if (::lstat(entry.path().c_str(), &info) != 0 ||
            !S_ISSOCK(info.st_mode) ||
            info.st_uid != current_uid) {
            continue;
        }

        const std::size_t number_begin = prefix.size();
        const std::size_t number_size = name.size() - prefix.size() - suffix.size();
        if (number_size == 0) {
            continue;
        }

        long parsed_pid = 0;
        const std::string_view number(name.data() + number_begin, number_size);
        const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed_pid);
        if (result.ec != std::errc {} || result.ptr != number.data() + number.size() || parsed_pid <= 0) {
            continue;
        }

        targets.push_back(Target {static_cast<pid_t>(parsed_pid), entry.path()});
    }

    std::sort(targets.begin(), targets.end(), [](const Target& left, const Target& right) {
        return left.pid < right.pid;
    });
    return targets;
}

std::optional<Target> find_target(pid_t pid) {
    const std::vector<Target> targets = discover_targets();
    const auto it = std::find_if(targets.begin(), targets.end(), [pid](const Target& target) {
        return target.pid == pid;
    });
    if (it == targets.end()) {
        return std::nullopt;
    }
    return *it;
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

bool request(const fs::path& socket_path,
             const std::string& command,
             std::string& response,
             std::string& error) {
    const std::string socket_string = socket_path.string();
    sockaddr_un address {};
    if (socket_string.size() >= sizeof(address.sun_path)) {
        error = "socket path is too long";
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_string.c_str());

    if (::connect(fd,
                  reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address))) != 0) {
        error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    if (!send_all(fd, command + "\n")) {
        error = std::string("send: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    response.clear();
    char buffer[1024];
    while (true) {
        const ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("recv: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (rc == 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(rc));
        if (response.size() > 16384) {
            error = "response exceeded safety limit";
            ::close(fd);
            return false;
        }
    }

    ::close(fd);
    return true;
}

void print_targets() {
    const std::vector<Target> targets = discover_targets();
    if (targets.empty()) {
        std::cout << "No cooperative load targets are running.\n";
        return;
    }

    std::cout << "Running cooperative load targets:\n";
    for (const Target& target : targets) {
        std::cout << "  PID " << static_cast<long>(target.pid)
                  << "  " << target.socket_path.string() << '\n';
    }
}

std::optional<pid_t> parse_pid(std::string_view text) {
    long value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || value <= 0) {
        return std::nullopt;
    }
    return static_cast<pid_t>(value);
}

std::optional<Target> choose_target_interactively() {
    const std::vector<Target> targets = discover_targets();
    if (targets.empty()) {
        std::cout << "No targets found. Start kirkware-load-target in another terminal first.\n";
        return std::nullopt;
    }

    std::cout << "\nKIRKWARE COOPERATIVE LOAD TEST\n"
              << "--------------------------------\n";
    for (std::size_t index = 0; index < targets.size(); ++index) {
        std::cout << "  " << (index + 1) << ". PID "
                  << static_cast<long>(targets[index].pid) << '\n';
    }
    std::cout << "  0. Exit\n\nSelect target: ";

    std::string input;
    if (!std::getline(std::cin, input)) {
        return std::nullopt;
    }

    unsigned long selection = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), selection);
    if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || selection == 0) {
        return std::nullopt;
    }
    if (selection > targets.size()) {
        std::cout << "Invalid selection.\n";
        return std::nullopt;
    }
    return targets[selection - 1];
}

void print_response(const std::string& response) {
    if (!response.empty()) {
        std::cout << response;
        if (response.back() != '\n') {
            std::cout << '\n';
        }
    }
}

int interactive_mode() {
    const std::optional<Target> selected = choose_target_interactively();
    if (!selected.has_value()) {
        return 0;
    }

    while (true) {
        std::cout << "\nTarget PID " << static_cast<long>(selected->pid) << "\n"
                  << "  1. Load test .so\n"
                  << "  2. Status\n"
                  << "  3. Unload module\n"
                  << "  4. Stop target\n"
                  << "  0. Exit client\n"
                  << "Select action: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) {
            return 0;
        }
        if (choice == "0") {
            return 0;
        }

        std::string command;
        if (choice == "1") {
            const fs::path suggested = fs::current_path() / "build-linux" / "libkirkware_load_test.so";
            std::cout << "Module path [" << suggested.string() << "]: ";
            std::string module_input;
            if (!std::getline(std::cin, module_input)) {
                return 0;
            }
            fs::path module = module_input.empty() ? suggested : fs::path(module_input);
            std::error_code ec;
            module = fs::absolute(module, ec);
            if (ec) {
                std::cout << "Could not resolve module path: " << ec.message() << '\n';
                continue;
            }
            command = "LOAD " + module.lexically_normal().string();
        } else if (choice == "2") {
            command = "STATUS";
        } else if (choice == "3") {
            command = "UNLOAD";
        } else if (choice == "4") {
            command = "QUIT";
        } else {
            std::cout << "Invalid selection.\n";
            continue;
        }

        std::string response;
        std::string error;
        if (!request(selected->socket_path, command, response, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        print_response(response);
        if (choice == "4") {
            return 0;
        }
    }
}

void print_help() {
    std::cout
        << "Usage:\n"
        << "  kirkware-load-client                         Interactive terminal UI\n"
        << "  kirkware-load-client --list                  List cooperative targets\n"
        << "  kirkware-load-client --target PID --status\n"
        << "  kirkware-load-client --target PID --load /absolute/module.so\n"
        << "  kirkware-load-client --target PID --unload\n"
        << "  kirkware-load-client --target PID --quit\n\n"
        << "This client never writes to another process. It asks a matching same-user\n"
        << "kirkware-load-target process to load the selected shared object itself.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return interactive_mode();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        print_targets();
        return 0;
    }

    std::optional<pid_t> target_pid;
    std::string command;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--target") {
            if (index + 1 >= argc) {
                std::cerr << "error: --target requires a PID\n";
                return 2;
            }
            target_pid = parse_pid(argv[++index]);
            if (!target_pid.has_value()) {
                std::cerr << "error: invalid PID\n";
                return 2;
            }
        } else if (argument == "--status") {
            command = "STATUS";
        } else if (argument == "--unload") {
            command = "UNLOAD";
        } else if (argument == "--quit") {
            command = "QUIT";
        } else if (argument == "--load") {
            if (index + 1 >= argc) {
                std::cerr << "error: --load requires a shared-object path\n";
                return 2;
            }
            std::error_code ec;
            fs::path module = fs::absolute(argv[++index], ec);
            if (ec) {
                std::cerr << "error: cannot resolve module path: " << ec.message() << '\n';
                return 2;
            }
            command = "LOAD " + module.lexically_normal().string();
        } else {
            std::cerr << "error: unknown option: " << argument << '\n';
            return 2;
        }
    }

    if (!target_pid.has_value() || command.empty()) {
        print_help();
        return 2;
    }

    const std::optional<Target> target = find_target(*target_pid);
    if (!target.has_value()) {
        std::cerr << "error: cooperative target PID " << static_cast<long>(*target_pid)
                  << " was not found\n";
        return 1;
    }

    std::string response;
    std::string error;
    if (!request(target->socket_path, command, response, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    print_response(response);
    return response.rfind("OK ", 0) == 0 ? 0 : 1;
}
