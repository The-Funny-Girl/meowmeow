#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Target {
    pid_t pid {};
    fs::path control_dir;
};

fs::path control_root() {
    const char* configured = std::getenv("KIRKWARE_LOAD_CONTROL_ROOT");
    if (configured == nullptr || configured[0] == '\0') {
        return fs::path("/tmp");
    }

    std::error_code ec;
    fs::path root = fs::absolute(fs::path(configured), ec);
    if (ec) {
        return fs::path(configured).lexically_normal();
    }
    return root.lexically_normal();
}

std::string target_prefix() {
    return "kirkware-load-target-" +
           std::to_string(static_cast<unsigned long>(::getuid())) + "-";
}

fs::path control_dir_for(pid_t pid) {
    return control_root() /
           (target_prefix() + std::to_string(static_cast<long>(pid)));
}

fs::path default_module_path() {
    std::error_code ec;
    const fs::path executable = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !executable.empty()) {
        return executable.parent_path() / "libkirkware_load_test.so";
    }
    return fs::current_path() / "build-load-harness" / "libkirkware_load_test.so";
}

std::optional<Target> target_from_pid(pid_t pid) {
    if (::kill(pid, 0) != 0 && errno != EPERM) {
        return std::nullopt;
    }

    const fs::path control_dir = control_dir_for(pid);
    struct stat info {};
    if (::lstat(control_dir.c_str(), &info) != 0 ||
        !S_ISDIR(info.st_mode) ||
        info.st_uid != ::getuid()) {
        return std::nullopt;
    }
    return Target {pid, control_dir};
}

std::vector<Target> discover_targets() {
    std::vector<Target> targets;
    const std::string prefix = target_prefix();

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(control_root(), ec)) {
        if (ec) {
            break;
        }

        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix)) {
            continue;
        }

        const std::string_view number(name.data() + prefix.size(), name.size() - prefix.size());
        if (number.empty()) {
            continue;
        }

        long parsed_pid = 0;
        const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed_pid);
        if (result.ec != std::errc {} ||
            result.ptr != number.data() + number.size() ||
            parsed_pid <= 0) {
            continue;
        }

        if (const auto target = target_from_pid(static_cast<pid_t>(parsed_pid)); target.has_value()) {
            targets.push_back(*target);
        }
    }

    std::sort(targets.begin(), targets.end(), [](const Target& left, const Target& right) {
        return left.pid < right.pid;
    });
    return targets;
}

std::optional<Target> find_target(pid_t pid) {
    return target_from_pid(pid);
}

std::string next_token() {
    static unsigned long counter = 0;
    ++counter;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long>(::getpid())) + "-" +
           std::to_string(counter) + "-" + std::to_string(stamp);
}

bool write_request(const Target& target,
                   const std::string& token,
                   const std::string& command,
                   std::string& error) {
    const fs::path temp_path = target.control_dir /
                               ("request." + std::to_string(static_cast<long>(::getpid())) + ".tmp");
    const fs::path request_path = target.control_dir / "request.txt";

    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output) {
            error = "cannot create request file";
            return false;
        }
        output << token << '\n' << command << '\n';
        output.flush();
        if (!output) {
            error = "cannot write request file";
            return false;
        }
    }

    if (::chmod(temp_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = std::string("cannot secure request file: ") + std::strerror(errno);
        return false;
    }

    std::error_code rename_ec;
    fs::rename(temp_path, request_path, rename_ec);
    if (rename_ec) {
        error = "cannot publish request file: " + rename_ec.message();
        return false;
    }
    return true;
}

bool read_matching_response(const Target& target,
                            const std::string& token,
                            std::string& response) {
    const fs::path response_path = target.control_dir / "response.txt";
    std::ifstream input(response_path);
    if (!input) {
        return false;
    }

    std::string response_token;
    if (!std::getline(input, response_token) || response_token != token) {
        return false;
    }

    std::ostringstream payload;
    payload << input.rdbuf();
    response = payload.str();
    input.close();

    std::error_code remove_ec;
    fs::remove(response_path, remove_ec);
    return true;
}

bool request(const Target& target,
             const std::string& command,
             std::string& response,
             std::string& error) {
    const std::string token = next_token();
    const fs::path response_path = target.control_dir / "response.txt";
    std::error_code remove_ec;
    fs::remove(response_path, remove_ec);

    if (!write_request(target, token, command, error)) {
        return false;
    }

    if (::kill(target.pid, SIGUSR1) != 0) {
        error = std::string("cannot signal target: ") + std::strerror(errno);
        return false;
    }

    for (int attempt = 0; attempt < 150; ++attempt) {
        if (read_matching_response(target, token, response)) {
            return true;
        }
        if (::kill(target.pid, 0) != 0 && errno != EPERM) {
            error = "target exited before replying";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    error = "target did not reply within 3 seconds";
    return false;
}

bool ping_target(const Target& target, std::string& error) {
    std::string response;
    if (!request(target, "PING", response, error)) {
        return false;
    }
    if (response != "OK pong\n") {
        error = "target returned an unexpected PING response";
        return false;
    }
    return true;
}

void print_targets() {
    const std::vector<Target> targets = discover_targets();
    if (targets.empty()) {
        std::cout << "No cooperative load targets are running under "
                  << control_root().string() << ".\n";
        return;
    }

    std::cout << "Running cooperative load targets under "
              << control_root().string() << ":\n";
    for (const Target& target : targets) {
        std::cout << "  PID " << static_cast<long>(target.pid)
                  << "  " << target.control_dir.string() << '\n';
    }
}

std::optional<pid_t> parse_pid(std::string_view text) {
    long value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} ||
        result.ptr != text.data() + text.size() ||
        value <= 0) {
        return std::nullopt;
    }
    return static_cast<pid_t>(value);
}

std::optional<Target> choose_target_interactively() {
    const std::vector<Target> targets = discover_targets();
    if (targets.empty()) {
        std::cout << "No targets found under " << control_root().string()
                  << ". Start kirkware-load-target with the same control root first.\n";
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

int interactive_target(const Target& selected) {
    std::string ping_error;
    if (!ping_target(selected, ping_error)) {
        std::cerr << "error: target PID " << static_cast<long>(selected.pid)
                  << " is not responding: " << ping_error << '\n';
        return 1;
    }

    while (true) {
        std::cout << "\nTarget PID " << static_cast<long>(selected.pid) << "\n"
                  << "Control root " << control_root().string() << "\n"
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
            const fs::path suggested = default_module_path();
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
        if (!request(selected, command, response, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        print_response(response);
        if (choice == "4") {
            return 0;
        }
    }
}

int interactive_mode() {
    const std::optional<Target> selected = choose_target_interactively();
    if (!selected.has_value()) {
        return 0;
    }
    return interactive_target(*selected);
}

void print_help() {
    std::cout
        << "Usage:\n"
        << "  kirkware-load-client                         Interactive target discovery UI\n"
        << "  kirkware-load-client --list                  List cooperative targets\n"
        << "  kirkware-load-client --target PID            Open UI directly for exact target PID\n"
        << "  kirkware-load-client --target PID --status\n"
        << "  kirkware-load-client --target PID --load /absolute/module.so\n"
        << "  kirkware-load-client --target PID --unload\n"
        << "  kirkware-load-client --target PID --quit\n\n"
        << "Transport: private per-PID control files plus SIGUSR1. No Unix socket is used.\n"
        << "Set KIRKWARE_LOAD_CONTROL_ROOT to choose the parent folder (default: /tmp).\n"
        << "The selected process must still be a cooperating kirkware-load-target process.\n";
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

    if (!target_pid.has_value()) {
        print_help();
        return 2;
    }

    const std::optional<Target> target = find_target(*target_pid);
    if (!target.has_value()) {
        std::cerr << "error: PID " << static_cast<long>(*target_pid)
                  << " is not a cooperative load target at "
                  << control_dir_for(*target_pid).string() << '\n';
        return 1;
    }

    if (command.empty()) {
        return interactive_target(*target);
    }

    std::string response;
    std::string error;
    if (!request(*target, command, response, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    print_response(response);
    return response.rfind("OK ", 0) == 0 ? 0 : 1;
}
