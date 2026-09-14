#include "application.hpp"
#include "linux_paths.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kVersion = "1.1.0-linux";

void PrintUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  --help             Show this help text\n"
        << "  --version          Print the Linux port version\n"
        << "  --paths            Print resolved Linux/XDG paths\n"
        << "  --check            Run filesystem/runtime health checks\n"
        << "  --no-workspace     Do not create a temporary workspace\n"
        << "  --keep-workspace   Keep the temporary workspace after exit\n";
}

} // namespace

int main(int argc, char** argv)
{
    bool show_paths = false;
    bool run_checks = false;
    kirkware::platform::ApplicationOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argument == "--version") {
            std::cout << "kirkware " << kVersion << '\n';
            return 0;
        }
        if (argument == "--paths") {
            show_paths = true;
            continue;
        }
        if (argument == "--check") {
            run_checks = true;
            continue;
        }
        if (argument == "--no-workspace") {
            options.create_workspace = false;
            continue;
        }
        if (argument == "--keep-workspace") {
            options.keep_workspace = true;
            continue;
        }
        std::cerr << "Unknown option: " << argument << "\n\n";
        PrintUsage(argv[0]);
        return 2;
    }

    kirkware::platform::Application application;
    std::string error;
    if (!application.Initialize(options, &error)) {
        std::cerr << "Initialization failed: " << error << '\n';
        return 1;
    }

    if (show_paths) {
        for (const auto& [name, path] :
             kirkware::platform::NamedPaths(application.paths())) {
            std::cout << name << '=' << path << '\n';
        }
    }

    if (run_checks) {
        bool all_ok = true;
        for (const auto& check : application.CheckHealth()) {
            std::cout << (check.ok ? "PASS" : "FAIL") << "  " << check.name
                      << "  " << check.detail << '\n';
            all_ok = all_ok && check.ok;
        }
        return all_ok ? 0 : 1;
    }

    if (!show_paths) {
        std::cout << "kirkware Linux runtime initialized\n";
        std::cout << "config: " << application.paths().config_root << '\n';
        if (application.workspace())
            std::cout << "workspace: " << application.workspace()->path() << '\n';
        std::cout << "log: " << application.paths().logs / "kirkware.log" << '\n';
    }
    return 0;
}
