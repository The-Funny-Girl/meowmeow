#include "kirkware_clean_traces.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace kirkware::clean_traces
{
namespace
{

namespace fs = std::filesystem;

class RegistryKey
{
public:
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey()
    {
        if (value_ != nullptr)
            RegCloseKey(value_);
    }

    HKEY* Put()
    {
        return &value_;
    }

    HKEY Get() const
    {
        return value_;
    }

private:
    HKEY value_ = nullptr;
};

std::wstring DecodeText(std::string_view value)
{
    if (value.empty())
        return {};
    int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0)
    {
        code_page = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(
            code_page, flags, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
    }
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            code_page, flags, value.data(),
            static_cast<int>(value.size()), result.data(), required) !=
        required)
    {
        return {};
    }
    return result;
}

std::vector<std::string> ReadQuotedTokens(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::vector<std::string> tokens;
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        if (bytes[index] != '"')
            continue;
        std::string token;
        ++index;
        while (index < bytes.size())
        {
            const char value = bytes[index];
            if (value == '"')
                break;
            if (value == '\\' && index + 1 < bytes.size() &&
                (bytes[index + 1] == '\\' || bytes[index + 1] == '"'))
            {
                token.push_back(bytes[index + 1]);
                index += 2;
                continue;
            }
            token.push_back(value);
            ++index;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool EqualAsciiInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        const unsigned char lower_a =
            a >= 'A' && a <= 'Z' ? static_cast<unsigned char>(a + 32) : a;
        const unsigned char lower_b =
            b >= 'A' && b <= 'Z' ? static_cast<unsigned char>(b + 32) : b;
        if (lower_a != lower_b)
            return false;
    }
    return true;
}

std::vector<std::wstring> ValuesForKey(const fs::path& path,
                                       std::string_view key)
{
    const std::vector<std::string> tokens = ReadQuotedTokens(path);
    std::vector<std::wstring> values;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index)
    {
        if (!EqualAsciiInsensitive(tokens[index], key))
            continue;
        std::wstring value = DecodeText(tokens[index + 1]);
        if (!value.empty())
            values.push_back(std::move(value));
        ++index;
    }
    return values;
}

std::optional<std::wstring> ValueForKey(const fs::path& path,
                                        std::string_view key)
{
    std::vector<std::wstring> values = ValuesForKey(path, key);
    if (values.empty())
        return std::nullopt;
    return std::move(values.front());
}

std::optional<fs::path> ReadSteamPath()
{
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE,
        key.Put());
    if (status != ERROR_SUCCESS)
        return std::nullopt;
    DWORD type = 0;
    DWORD bytes = 0;
    status = RegQueryValueExW(
        key.Get(), L"SteamPath", nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ ||
        bytes < sizeof(wchar_t))
    {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(bytes / sizeof(wchar_t)) + 1, L'\0');
    status = RegQueryValueExW(
        key.Get(), L"SteamPath", nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS || buffer.front() == L'\0')
        return std::nullopt;
    return fs::path(buffer.data());
}

std::optional<fs::path> FindInstallRoot()
{
    const std::optional<fs::path> steam = ReadSteamPath();
    if (!steam)
        return std::nullopt;
    const fs::path folders =
        *steam / L"SteamApps" / L"libraryfolders.vdf";
    const std::vector<std::wstring> library_values =
        ValuesForKey(folders, "path");
    std::vector<fs::path> libraries;
    libraries.push_back(*steam);
    for (const std::wstring& library_value : library_values)
        libraries.emplace_back(library_value);
    for (const fs::path& library : libraries)
    {
        const fs::path manifest =
            library / L"SteamApps" / L"appmanifest_4000.acf";
        if (!fs::exists(manifest))
            continue;
        const std::optional<std::wstring> install_directory =
            ValueForKey(manifest, "installdir");
        if (!install_directory)
            return std::nullopt;
        return manifest.parent_path() / L"common" / *install_directory;
    }
    return std::nullopt;
}

std::optional<fs::path> ReadLocalAppDataRoot()
{
    std::array<wchar_t, MAX_PATH> value{};
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size())
        return std::nullopt;
    return fs::path(value.data()) / L"Garry's Mod";
}

void RemoveOne(const fs::path& path)
{
    if (fs::exists(path))
        fs::remove(path);
}

void RemoveTree(const fs::path& path)
{
    if (fs::exists(path))
        fs::remove_all(path);
}

void RemoveCfg(const fs::path& content_root)
{
    const fs::path cfg = content_root / L"cfg";
    if (!fs::exists(cfg))
        return;
    for (const fs::directory_entry& entry : fs::directory_iterator(cfg))
    {
        if (entry.path().filename() != L"autoexec.cfg")
            RemoveOne(entry.path());
    }
}

void RemoveDumps(const fs::path& root)
{
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    while (iterator != end)
    {
        if (!error && iterator->path().extension() == L".dmp")
            RemoveOne(iterator->path());
        iterator.increment(error);
        if (error)
            error.clear();
    }
}

class TimeRandomizer
{
public:
    TimeRandomizer()
        : engine_(std::random_device{}())
    {
    }

    explicit TimeRandomizer(std::uint32_t seed)
        : engine_(seed)
    {
    }

    fs::file_time_type::duration Next()
    {
        const auto value =
            std::chrono::hours(hours_(engine_)) +
            std::chrono::minutes(minutes_(engine_)) +
            std::chrono::seconds(seconds_(engine_)) +
            std::chrono::milliseconds(milliseconds_(engine_)) +
            std::chrono::microseconds(microseconds_(engine_)) +
            std::chrono::nanoseconds(nanoseconds_(engine_));
        return std::chrono::duration_cast<fs::file_time_type::duration>(value);
    }

private:
    std::mt19937 engine_;
    std::uniform_int_distribution<int> hours_{1, 168};
    std::uniform_int_distribution<int> minutes_{0, 59};
    std::uniform_int_distribution<int> seconds_{0, 59};
    std::uniform_int_distribution<int> milliseconds_{0, 999};
    std::uniform_int_distribution<int> microseconds_{0, 999};
    std::uniform_int_distribution<int> nanoseconds_{0, 999};
};

void Backdate(const fs::path& root, TimeRandomizer& randomizer)
{
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    while (iterator != end)
    {
        if (!error)
        {
            const fs::path path = iterator->path();
            const fs::file_time_type previous = fs::last_write_time(path, error);
            if (!error)
                fs::last_write_time(path, previous - randomizer.Next(), error);
        }
        iterator.increment(error);
        if (error)
            error.clear();
    }
}

bool RunAtRootsInternal(const fs::path& install_root,
                        const std::optional<fs::path>& local_root,
                        bool discover_local_root,
                        const std::optional<std::uint32_t>& seed)
{
    if (!fs::exists(install_root))
        return true;
    const fs::path content_root = install_root / L"garrysmod";
    RemoveCfg(content_root);
    RemoveOne(content_root / L"cl.db");
    RemoveOne(content_root / L"mn.db");
    RemoveOne(content_root / L"sv.db");
    RemoveOne(content_root / L"voice_ban.dt");
    RemoveTree(content_root / L"screenshots");
    RemoveTree(content_root / L"data");
    RemoveTree(content_root / L"demos");
    RemoveTree(content_root / L"cache");
    RemoveTree(content_root / L"download");
    RemoveTree(content_root / L"garrysmod" / L"cache");
    RemoveTree(content_root / L"crashes");
    RemoveTree(install_root / L"crashes");
    RemoveDumps(install_root);
    RemoveDumps(content_root);
    std::optional<TimeRandomizer> randomizer;
    if (seed)
        randomizer.emplace(*seed);
    else
        randomizer.emplace();
    Backdate(install_root, *randomizer);
    const std::optional<fs::path> resolved_local_root =
        discover_local_root ? ReadLocalAppDataRoot() : local_root;
    if (resolved_local_root && fs::exists(*resolved_local_root))
    {
        RemoveTree(*resolved_local_root / L"cache");
        RemoveTree(*resolved_local_root / L"download");
        RemoveTree(*resolved_local_root / L"crashes");
        RemoveDumps(*resolved_local_root);
        Backdate(*resolved_local_root, *randomizer);
    }
    return true;
}

}

bool Run()
{
    try
    {
        const std::optional<fs::path> install_root = FindInstallRoot();
        if (!install_root)
            return true;
        return RunAtRootsInternal(
            *install_root, std::nullopt, true, std::nullopt);
    }
    catch (...)
    {
        return false;
    }
}

}
