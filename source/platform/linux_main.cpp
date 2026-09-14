#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path ConfigRoot()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "kirkware";

    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "kirkware";

    return {};
}

bool EnsureDirectoryTree(const fs::path& root)
{
    if (root.empty())
        return false;

    std::error_code ec;
    fs::create_directories(root / "configs", ec);
    if (ec)
        return false;
    fs::create_directories(root / "luas", ec);
    if (ec)
        return false;
    fs::create_directories(root / "autorun" / "client", ec);
    if (ec)
        return false;
    fs::create_directories(root / "autorun" / "menu", ec);
    if (ec)
        return false;
    fs::create_directories(root / "dumps", ec);
    if (ec)
        return false;
    fs::create_directories(root / "workspace", ec);
    if (ec)
        return false;
    fs::create_directories(root / "logger", ec);
    if (ec)
        return false;
    fs::create_directories(root / "automations", ec);
    return !ec;
}

} // namespace

int main()
{
    const fs::path root = ConfigRoot();
    if (!EnsureDirectoryTree(root)) {
        std::cerr << "Unable to create Linux configuration tree" << std::endl;
        return 1;
    }

    std::cout << "kirkware Linux port initialized at " << root << '\n';
    return 0;
}
