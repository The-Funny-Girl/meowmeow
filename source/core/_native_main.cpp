#include "kirkware_native_chain.hpp"
#include "gmod_main_menu_gate.hpp"
#include "kirkware_clean_traces.hpp"
#include "kirkware_config_sanitizer.hpp"
#include "kirkware_import_rebuilder.hpp"
#include "kirkware_network_guard.hpp"
#include "kirkware_scoped_early_auth_hook.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

int kirkware_bootstrap_entry(int, wchar_t**);
int kirkware_handoff_entry(int, char**);
int kirkware_envelope_entry(int, char**);
int kirkware_swap_entry(int, char**);
int kirkware_worker_idle_entry(int, char**);
int kirkware_steam_offline_guard_entry(int, char**);
int kirkware_context_entry(int, char**);
int kirkware_lua_resolver_entry(int, char**);
int kirkware_veh_entry(int, char**);
int kirkware_locale_entry(int, char**);
int kirkware_interfaces_entry(int, char**);
int kirkware_game_hooks_entry(int, char**);
int kirkware_carrier_entry(int, char**);

namespace {

constexpr int kActiveImageResource = 401;
constexpr int kBootstrapB1Resource = 402;
constexpr int kBootstrapB2Resource = 403;
constexpr int kBssResource = 404;
constexpr int kBssMaskResource = 405;
constexpr int kLocaleResource = 406;
constexpr std::uint64_t kImageBase = 0x1E5DCC00000ull;
constexpr std::uint64_t kBootstrapB2Base = 0x1E5DF940000ull;
constexpr char kBootstrapB2Hash[] =
    "5281D7D5F6D6D70F2F912281DD9E05169683898A2735452E87C07B0E82FDF143";
constexpr std::uint64_t kBootstrapImageSize = 18227712;
constexpr int kProtectedEntryPending = INT_MIN;
constexpr std::uint64_t kHandoffUnixOffset = 0x228;
constexpr char kProtectedEntryArgumentMode[] = "context";
std::atomic<bool> g_run_garrys_mod_requested{false};

bool EnsureDirectory(const wchar_t* path) {
    if (CreateDirectoryW(path, nullptr))
        return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool EnsureProductTree(const wchar_t* root) {
    const std::wstring base(root);
    return EnsureDirectory(root) &&
           EnsureDirectory((base + L"\\configs").c_str()) &&
           EnsureDirectory((base + L"\\luas").c_str()) &&
           EnsureDirectory((base + L"\\luas\\autorun").c_str()) &&
           EnsureDirectory((base + L"\\luas\\autorun\\client").c_str()) &&
           EnsureDirectory((base + L"\\luas\\autorun\\menu").c_str()) &&
           EnsureDirectory((base + L"\\dumps").c_str()) &&
           EnsureDirectory((base + L"\\workspace").c_str()) &&
           EnsureDirectory((base + L"\\logger").c_str()) &&
           EnsureDirectory((base + L"\\automations").c_str());
}

bool EnsureConfigDirectory() {
    return EnsureProductTree(L"C:\\kirkware");
}

bool EnsurePrivateDirectory(const std::string& path) {
    if (!CreateDirectoryA(path.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool BuildBootstrapCachePath(std::string& path) {
    char local[MAX_PATH + 1]{};
    const DWORD length = GetEnvironmentVariableA(
        "LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    if (length == 0 || length >= std::size(local))
        return false;
    std::string root(local, length);
    while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();
    const std::string product = root + "\\kirkware";
    const std::string carriers = product + "\\carriers";
    const std::string version = carriers + "\\" + kBootstrapB2Hash;
    if (!EnsurePrivateDirectory(product) ||
        !EnsurePrivateDirectory(carriers) ||
        !EnsurePrivateDirectory(version))
        return false;
    path = version + "\\bootstrap-b2.dll";
    return path.size() < MAX_PATH;
}

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    void Reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }
    HANDLE Get() const { return value_; }
    explicit operator bool() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_ = nullptr;
};

bool NormalizeKirkwareConfigFile(const std::wstring& path) {
    const auto result = kirkware::config_sanitizer::SanitizeFile(path);
    return result == kirkware::config_sanitizer::Result::Unchanged ||
           result == kirkware::config_sanitizer::Result::Rewritten ||
           result == kirkware::config_sanitizer::Result::Quarantined ||
           result == kirkware::config_sanitizer::Result::IoError;
}

bool NormalizeConfigsIn(const wchar_t* root, const wchar_t* glob) {
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(glob, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool success = true;
    do {
        if ((entry.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            const std::wstring path = std::wstring(root) + entry.cFileName;
            if (!NormalizeKirkwareConfigFile(path)) {
                success = false;
                break;
            }
        }
    } while (FindNextFileW(search, &entry));
    const DWORD error = GetLastError();
    FindClose(search);
    return success && error == ERROR_NO_MORE_FILES;
}

bool NormalizeLegacyPlaintextConfigs() {
    return NormalizeConfigsIn(L"C:\\kirkware\\configs\\",
                              L"C:\\kirkware\\configs\\*.ss");
}

struct ProtectedLifecycle {
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle abort{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle success{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    bool Valid() const {
        return static_cast<bool>(ready) && static_cast<bool>(release) &&
               static_cast<bool>(abort) && static_cast<bool>(success) &&
               static_cast<bool>(done);
    }
};

class ScopedEnvironmentOverride {
  public:
    ScopedEnvironmentOverride(std::string name, std::string value)
        : name_(std::move(name)) {
        const char* current = std::getenv(name_.c_str());
        if (current) {
            had_previous_ = true;
            previous_ = current;
        }
        active_ = _putenv_s(name_.c_str(), value.c_str()) == 0;
    }

    ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
    ScopedEnvironmentOverride& operator=(
        const ScopedEnvironmentOverride&) = delete;

    ~ScopedEnvironmentOverride() {
        if (active_)
            Restore();
    }

    bool Active() const { return active_; }

    bool Restore() {
        if (!active_)
            return true;
        if (_putenv_s(name_.c_str(),
                      had_previous_ ? previous_.c_str() : "") != 0)
            return false;
        active_ = false;
        return true;
    }

  private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
    bool active_ = false;
};

std::mutex& ProtectedEnvironmentMutex() {
    static std::mutex mutex;
    return mutex;
}

bool ReadHandoffUnixTime(const std::string& path, std::uint32_t& value) {
    Handle file(CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    if (!file)
        return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(kHandoffUnixOffset);
    if (!SetFilePointerEx(file.Get(), position, nullptr, FILE_BEGIN))
        return false;
    std::array<std::uint8_t, sizeof(std::uint32_t)> bytes{};
    DWORD read = 0;
    if (!ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                  &read, nullptr) ||
        read != bytes.size())
        return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
    return value != 0;
}

constexpr std::array<std::string_view, 9> kWorkspaceFileNames{{
    "active-image-b1.bin",
    "bootstrap-b1.dll",
    "bootstrap-b2.dll",
    "reconstructed-bss.bin",
    "reconstructed-bss.mask.bin",
    "mbcinfo-template.bin",
    "handoff.bin",
    "envelope.bin",
    "workspace.lock"}};

bool WorkspaceNameValid(std::string_view name) {
    if (name.size() < 8 || name.substr(0, 3) != "KWP" ||
        name.substr(name.size() - 4) != ".tmp")
        return false;
    const std::size_t digits = name.size() - 7;
    if ((digits == 0 || digits > 4) && digits != 32)
        return false;
    for (std::size_t index = 3; index + 4 < name.size(); ++index) {
        const char value = name[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'F') ||
              (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

bool WorkspaceFileNameValid(std::string_view name) {
    return std::find(kWorkspaceFileNames.begin(), kWorkspaceFileNames.end(),
                     name) != kWorkspaceFileNames.end();
}

bool FileMissing(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool ScrubFile(const std::string& path) {
    Handle file(CreateFileA(
        path.c_str(), GENERIC_WRITE | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!file)
        return FileMissing(path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0)
        return false;
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file.Get(), start, nullptr, FILE_BEGIN))
        return false;
    std::array<std::uint8_t, 65536> zeros{};
    LONGLONG remaining = size.QuadPart;
    while (remaining > 0) {
        const DWORD requested = static_cast<DWORD>(std::min<LONGLONG>(
            remaining, static_cast<LONGLONG>(zeros.size())));
        DWORD written = 0;
        if (!WriteFile(file.Get(), zeros.data(), requested, &written,
                       nullptr) ||
            written != requested)
            return false;
        remaining -= written;
    }
    if (!FlushFileBuffers(file.Get()) ||
        !SetFilePointerEx(file.Get(), start, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file.Get()) || !FlushFileBuffers(file.Get()))
        return false;
    return true;
}

bool DeleteWorkspaceFile(const std::string& path, bool sensitive) {
    for (int attempt = 0; attempt != 20; ++attempt) {
        if (sensitive)
            ScrubFile(path);
        SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileA(path.c_str()) || FileMissing(path))
            return true;
        Sleep(25);
    }
    return FileMissing(path);
}

bool WorkspaceFingerprintValid(const std::string& root) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const auto exact_size = [&root, &data](const char* name,
                                           std::uint64_t expected) {
        const std::string path = root + "\\" + name;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard,
                                  &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;
        const std::uint64_t size =
            (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
        return size == expected;
    };
    return exact_size("handoff.bin", 0x30240) ||
           exact_size("envelope.bin", 0x30298) ||
           exact_size("active-image-b1.bin", 0x1162000) ||
           exact_size("bootstrap-b1.dll", kBootstrapImageSize) ||
           exact_size("bootstrap-b2.dll", kBootstrapImageSize);
}

bool WorkspaceContainsOnlyKnownFiles(const std::string& root) {
    const std::string pattern = root + "\\*";
    WIN32_FIND_DATAA data{};
    HANDLE search = FindFirstFileA(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool valid = true;
    do {
        const std::string_view name(data.cFileName);
        if (name == "." || name == "..")
            continue;
        if ((data.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            !WorkspaceFileNameValid(name)) {
            valid = false;
            break;
        }
    } while (FindNextFileA(search, &data));
    FindClose(search);
    return valid;
}

bool WorkspaceLockActive(const std::string& root) {
    const std::string path = root + "\\workspace.lock";
    Handle lock(CreateFileA(path.c_str(), GENERIC_READ | DELETE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    if (lock)
        return false;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return false;
    return true;
}

bool CleanupWorkspaceDirectory(const std::string& root, bool stale) {
    if (root.empty() ||
        (stale && (!WorkspaceFingerprintValid(root) ||
                   !WorkspaceContainsOnlyKnownFiles(root) ||
                   WorkspaceLockActive(root))))
        return false;
    bool files_removed = true;
    for (const std::string_view name : kWorkspaceFileNames) {
        const bool sensitive = name == "handoff.bin" ||
                               name == "envelope.bin";
        files_removed =
            DeleteWorkspaceFile(root + "\\" + std::string(name), sensitive) &&
            files_removed;
    }
    if (!files_removed)
        return false;
    for (int attempt = 0; attempt != 20; ++attempt) {
        SetFileAttributesA(root.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryA(root.c_str()) || FileMissing(root))
            return true;
        Sleep(25);
    }
    return FileMissing(root);
}

void CleanupStaleWorkspaces() {
    char temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathA(MAX_PATH, temp);
    if (length == 0 || length > MAX_PATH)
        return;
    const std::string pattern = std::string(temp) + "KWP*.tmp";
    WIN32_FIND_DATAA data{};
    HANDLE search = FindFirstFileA(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::string> roots;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
            WorkspaceNameValid(data.cFileName))
            roots.push_back(std::string(temp) + data.cFileName);
    } while (FindNextFileA(search, &data));
    FindClose(search);
    for (const auto& root : roots)
        CleanupWorkspaceDirectory(root, true);
}

class Workspace {
  public:
    Workspace() = default;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    ~Workspace() { Cleanup(); }

    static void CleanupStale() {
        static std::once_flag once;
        std::call_once(once, CleanupStaleWorkspaces);
    }

    bool Create() {
        char root[MAX_PATH + 1]{};
        const DWORD root_length = GetTempPathA(
            static_cast<DWORD>(std::size(root)), root);
        if (root_length == 0 || root_length >= std::size(root))
            return false;
        constexpr char hex[] = "0123456789ABCDEF";
        std::array<std::uint8_t, 16> random{};
        for (int attempt = 0; attempt != 64 && root_.empty(); ++attempt) {
            if (BCryptGenRandom(nullptr, random.data(),
                                static_cast<ULONG>(random.size()),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return false;
            std::string candidate(root, root_length);
            candidate += "KWP";
            for (const std::uint8_t value : random) {
                candidate.push_back(hex[value >> 4]);
                candidate.push_back(hex[value & 0x0F]);
            }
            candidate += ".tmp";
            if (candidate.size() >= MAX_PATH)
                return false;
            if (CreateDirectoryA(candidate.c_str(), nullptr))
                root_ = std::move(candidate);
            else if (GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        if (root_.empty())
            return false;
        active_ = root_ + "\\active-image-b1.bin";
        b1_ = root_ + "\\bootstrap-b1.dll";
        if (!BuildBootstrapCachePath(b2_)) {
            CleanupWorkspaceDirectory(root_, false);
            root_.clear();
            return false;
        }
        bss_ = root_ + "\\reconstructed-bss.bin";
        mask_ = root_ + "\\reconstructed-bss.mask.bin";
        locale_ = root_ + "\\mbcinfo-template.bin";
        handoff_ = root_ + "\\handoff.bin";
        envelope_ = root_ + "\\envelope.bin";
        const std::string lock_path = root_ + "\\workspace.lock";
        owner_.Reset(CreateFileA(
            lock_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
            nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY |
                FILE_FLAG_DELETE_ON_CLOSE,
            nullptr));
        if (!owner_) {
            CleanupWorkspaceDirectory(root_, false);
            root_.clear();
            return false;
        }
        return true;
    }

    bool Cleanup() {
        if (root_.empty())
            return true;
        owner_.Reset();
        const bool removed = CleanupWorkspaceDirectory(root_, false);
        if (removed)
            root_.clear();
        return removed;
    }

    const std::string& Active() const { return active_; }
    const std::string& B1() const { return b1_; }
    const std::string& B2() const { return b2_; }
    const std::string& Bss() const { return bss_; }
    const std::string& Mask() const { return mask_; }
    const std::string& Locale() const { return locale_; }
    const std::string& Handoff() const { return handoff_; }
    const std::string& Envelope() const { return envelope_; }

  private:
    Handle owner_;
    std::string root_;
    std::string active_;
    std::string b1_;
    std::string b2_;
    std::string bss_;
    std::string mask_;
    std::string locale_;
    std::string handoff_;
    std::string envelope_;
};

int Invoke(int (*entry)(int, char**), std::vector<std::string> values) {
    std::vector<char*> arguments;
    arguments.reserve(values.size());
    for (auto& value : values)
        arguments.push_back(value.data());
    return entry(static_cast<int>(arguments.size()), arguments.data());
}

std::string BuildBarrierArgument(HANDLE ready, HANDLE release,
                                 HANDLE abort, HANDLE success) {
    char value[160]{};
    const int length = std::snprintf(
        value, sizeof(value), "barrier:%llX:%llX:%llX:%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(ready)),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(release)),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(abort)),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(success)));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(value))
        return {};
    return value;
}

int InvokeWide(int (*entry)(int, wchar_t**),
               std::vector<std::wstring> values) {
    std::vector<wchar_t*> arguments;
    arguments.reserve(values.size());
    for (auto& value : values)
        arguments.push_back(value.data());
    return entry(static_cast<int>(arguments.size()), arguments.data());
}

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty())
        return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), result.data(), required,
            nullptr, nullptr) != required)
        return {};
    return result;
}

std::wstring AnsiToWide(std::string_view input) {
    if (input.empty())
        return {};
    const int required = MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, MB_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), result.data(), required) !=
        required)
        return {};
    return result;
}

bool UsernameValid(std::string_view username) {
    return !username.empty() && username.size() <= 20;
}

bool WriteResource(HINSTANCE instance, int identifier,
                   const std::string& path,
                   DWORD creation = CREATE_ALWAYS,
                   DWORD attributes = FILE_ATTRIBUTE_HIDDEN |
                                      FILE_ATTRIBUTE_TEMPORARY) {
    HRSRC information = FindResourceW(
        instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    HGLOBAL resource = information ? LoadResource(instance, information)
                                   : nullptr;
    const DWORD size = information ? SizeofResource(instance, information) : 0;
    const void* bytes = resource ? LockResource(resource) : nullptr;
    if (!bytes || !size)
        return false;
    Handle output(CreateFileA(
        path.c_str(), GENERIC_WRITE, 0, nullptr, creation, attributes,
        nullptr));
    if (!output)
        return false;
    DWORD written = 0;
    return WriteFile(output.Get(), bytes, size, &written, nullptr) &&
           written == size && FlushFileBuffers(output.Get());
}

bool ResourceHandleMatches(HINSTANCE instance, int identifier,
                           HANDLE input) {
    HRSRC information = FindResourceW(
        instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    HGLOBAL resource = information ? LoadResource(instance, information)
                                   : nullptr;
    const DWORD size = information ? SizeofResource(instance, information) : 0;
    const auto* bytes = resource
                            ? static_cast<const std::uint8_t*>(
                                  LockResource(resource))
                            : nullptr;
    if (!input || input == INVALID_HANDLE_VALUE || !bytes || !size)
        return false;
    LARGE_INTEGER file_size{};
    LARGE_INTEGER start{};
    if (!GetFileSizeEx(input, &file_size) || file_size.QuadPart != size ||
        !SetFilePointerEx(input, start, nullptr, FILE_BEGIN))
        return false;
    std::array<std::uint8_t, 65536> buffer{};
    DWORD offset = 0;
    while (offset != size) {
        const DWORD requested =
            std::min<DWORD>(size - offset,
                            static_cast<DWORD>(buffer.size()));
        DWORD received = 0;
        if (!ReadFile(input, buffer.data(), requested, &received, nullptr) ||
            received != requested ||
            std::memcmp(buffer.data(), bytes + offset, requested) != 0)
            return false;
        offset += received;
    }
    return SetFilePointerEx(input, start, nullptr, FILE_BEGIN) != FALSE;
}

bool EnsureCachedResource(HINSTANCE instance, int identifier,
                          const std::string& path, Handle& guard) {
    for (int attempt = 0; attempt != 20; ++attempt) {
        guard.Reset(CreateFileA(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (guard) {
            if (ResourceHandleMatches(instance, identifier, guard.Get()))
                return true;
            guard.Reset();
        }
        const DWORD file_attributes = GetFileAttributesA(path.c_str());
        if (file_attributes != INVALID_FILE_ATTRIBUTES) {
            if ((file_attributes &
                 (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
                0)
                return false;
            SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileA(path.c_str())) {
                Sleep(25);
                continue;
            }
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND &&
                error != ERROR_PATH_NOT_FOUND)
                return false;
        }
        WriteResource(instance, identifier, path, CREATE_NEW,
                      FILE_ATTRIBUTE_HIDDEN);
        Sleep(25);
    }
    return false;
}

template <typename T>
bool ReadRemote(HANDLE process, std::uint64_t address, T& value) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             &value, sizeof(value), &received) != FALSE &&
           received == sizeof(value);
}

bool ReadRemoteBytes(HANDLE process, std::uint64_t address, void* output,
                     std::size_t size) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             output, size, &received) != FALSE &&
           received == size;
}

bool ReadRemoteU32(DWORD pid, std::uint64_t address,
                   std::uint32_t& value) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    return process && ReadRemote(process.Get(), address, value);
}

bool RegionExecutable(DWORD protection);
bool SpanHasProtection(HANDLE process, std::uint64_t address,
                       std::size_t size, bool executable);

constexpr std::uint64_t kConfigDefaultBoolCount = 0xA3;
constexpr std::array<std::string_view, 18> kConfigBoolPrewarmKeys{{
    "entities_index",
    "esp_other_crosshair",
    "esp_other_crosshair_outline",
    "esp_player_distance",
    "esp_player_hardcoded_bbox",
    "esp_player_simulation",
    "esp_player_teambased",
    "esp_player_velocity",
    "exploits_cusercmd_override",
    "exploits_cusercmd_restoration",
    "exploits_fake_latency",
    "misc_block_luacmd",
    "misc_faststop",
    "playerlist_kirkware",
    "rage_antiaim_fake_enable",
    "exploits_rapidfire",
    "exploits_tickbase_shift_fakecommands",
    "exploits_tickbase_set_uncharge_rate",
}};

struct ConfigBoolRegistryState {
    std::uint64_t count = 0;
    std::size_t prewarm_key_count = 0;
};

bool ReadCompatibleConfigBoolRegistry(DWORD pid, std::uint64_t image_base,
                                      ConfigBoolRegistryState& state) {
    state = {};
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return false;
    std::uint64_t sentinel = 0;
    if (!ReadRemote(process.Get(), image_base + 0xB6E208, sentinel) ||
        !ReadRemote(process.Get(), image_base + 0xB6E210, state.count))
        return false;
    constexpr std::uint64_t maximum_count =
        kConfigDefaultBoolCount + kConfigBoolPrewarmKeys.size();
    if (state.count > maximum_count)
        return false;
    if (!sentinel)
        return state.count == 0;
    if (!SpanHasProtection(process.Get(), sentinel, 16, false))
        return false;
    std::uint64_t node = 0;
    std::uint64_t sentinel_previous = 0;
    if (!ReadRemote(process.Get(), sentinel, node) ||
        !ReadRemote(process.Get(), sentinel + 8, sentinel_previous))
        return false;
    std::uint64_t previous = sentinel;
    std::vector<std::uint64_t> nodes;
    std::vector<std::string> keys;
    nodes.reserve(static_cast<std::size_t>(state.count));
    keys.reserve(static_cast<std::size_t>(state.count));
    while (node != sentinel) {
        if (!node || nodes.size() >= state.count ||
            !SpanHasProtection(process.Get(), node, 0x34, false) ||
            std::find(nodes.begin(), nodes.end(), node) != nodes.end())
            return false;
        std::uint64_t next = 0;
        std::uint64_t node_previous = 0;
        std::uint64_t length = 0;
        std::uint64_t capacity = 0;
        if (!ReadRemote(process.Get(), node, next) ||
            !ReadRemote(process.Get(), node + 8, node_previous) ||
            !ReadRemote(process.Get(), node + 0x20, length) ||
            !ReadRemote(process.Get(), node + 0x28, capacity) ||
            node_previous != previous || length > 256 || capacity < length)
            return false;
        std::uint64_t text = node + 0x10;
        if (capacity > 15 &&
            (!ReadRemote(process.Get(), node + 0x10, text) || !text))
            return false;
        std::string key(static_cast<std::size_t>(length), '\0');
        if (length &&
            !ReadRemoteBytes(process.Get(), text, key.data(),
                             static_cast<std::size_t>(length)))
            return false;
        if (std::find(keys.begin(), keys.end(), key) != keys.end())
            return false;
        if (std::find(kConfigBoolPrewarmKeys.begin(),
                      kConfigBoolPrewarmKeys.end(),
                      std::string_view(key)) !=
            kConfigBoolPrewarmKeys.end())
            ++state.prewarm_key_count;
        nodes.push_back(node);
        keys.push_back(std::move(key));
        previous = node;
        node = next;
    }
    if (nodes.size() != state.count || sentinel_previous != previous)
        return false;
    if (state.count == 0)
        return state.prewarm_key_count == 0;
    return state.count ==
           kConfigDefaultBoolCount + state.prewarm_key_count;
}

bool WriteRemoteQword(HANDLE process, std::uint64_t address,
                      std::uint64_t value) {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                       &information, sizeof(information)) !=
            sizeof(information) ||
        information.State != MEM_COMMIT)
        return false;
    DWORD temporary = PAGE_READWRITE;
    if (RegionExecutable(information.Protect))
        temporary = PAGE_EXECUTE_READWRITE;
    DWORD previous = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address),
                          sizeof(value), temporary, &previous))
        return false;
    SIZE_T written = 0;
    const bool wrote =
        WriteProcessMemory(process, reinterpret_cast<void*>(address), &value,
                           sizeof(value), &written) != FALSE &&
        written == sizeof(value);
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process, reinterpret_cast<void*>(address),
                         sizeof(value), previous, &ignored) != FALSE;
    std::uint64_t observed = 0;
    return wrote && restored &&
           ReadRemote(process, address, observed) && observed == value;
}

bool B2AuxiliaryFailure(DWORD, int* failure_code, int code,
                        std::size_t index, std::uint64_t rva,
                        std::uint64_t value = 0) {
    if (failure_code)
        *failure_code = code;
    static_cast<void>(index);
    static_cast<void>(rva);
    static_cast<void>(value);
    return false;
}

bool RestoreB2LoadConfigSlots(DWORD pid, HANDLE process,
                              int* failure_code) {
    struct Slot {
        std::uint32_t rva;
        std::uint32_t target_rva;
    };
    constexpr std::array<Slot, 6> slots{{
        {0x58FA90, 0x005BB0},
        {0x58FA98, 0x005BB0},
        {0x58FAA0, 0x56AEE0},
        {0x58FAA8, 0x56AF00},
        {0x58FAB0, 0x56AF00},
        {0x58FAC0, 0x56B4D0},
    }};
    std::array<std::uint64_t, slots.size()> previous{};
    std::array<bool, slots.size()> changed{};
    for (std::size_t index = 0; index != slots.size(); ++index) {
        const std::uint64_t target =
            kBootstrapB2Base + slots[index].target_rva;
        if (!SpanHasProtection(process, target, 1, true))
            return B2AuxiliaryFailure(
                pid, failure_code, 52011 + static_cast<int>(index),
                index + 1, slots[index].target_rva, target);
        if (!ReadRemote(process, kBootstrapB2Base + slots[index].rva,
                        previous[index]))
            return B2AuxiliaryFailure(
                pid, failure_code, 52021 + static_cast<int>(index),
                index + 1, slots[index].rva, GetLastError());
    }
    std::uint64_t reserved = 1;
    if (!ReadRemote(process, kBootstrapB2Base + 0x58FAB8, reserved))
        return B2AuxiliaryFailure(pid, failure_code, 52031, 1,
                                  0x58FAB8, GetLastError());
    if (reserved != 0 &&
        !SpanHasProtection(process, reserved, 1, true))
        return B2AuxiliaryFailure(pid, failure_code, 52032, 1,
                                  0x58FAB8, reserved);
    if (!ReadRemote(process, kBootstrapB2Base + 0x58FAC8, reserved))
        return B2AuxiliaryFailure(pid, failure_code, 52033, 2,
                                  0x58FAC8, GetLastError());
    if (reserved != 0)
        return B2AuxiliaryFailure(pid, failure_code, 52034, 2,
                                  0x58FAC8, reserved);
    for (std::size_t index = 0; index != slots.size(); ++index) {
        const std::uint64_t target =
            kBootstrapB2Base + slots[index].target_rva;
        if (previous[index] == target)
            continue;
        if (!WriteRemoteQword(process,
                              kBootstrapB2Base + slots[index].rva,
                              target)) {
            for (std::size_t rollback = index; rollback != 0; --rollback) {
                const std::size_t prior = rollback - 1;
                if (changed[prior])
                    WriteRemoteQword(
                        process, kBootstrapB2Base + slots[prior].rva,
                        previous[prior]);
            }
            return B2AuxiliaryFailure(
                pid, failure_code, 52041 + static_cast<int>(index),
                index + 1, slots[index].rva, GetLastError());
        }
        changed[index] = true;
    }
    return true;
}

bool MirrorB2AuxiliaryImports(DWORD pid, int* failure_code) {
    struct State {
        std::uint64_t rva = 0;
        std::uint64_t source = 0;
        std::uint64_t previous = 0;
        bool changed = false;
    };
    std::array<State, std::size(kirkware::auxiliary_patches)> states{};
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                                   PROCESS_VM_OPERATION |
                                   PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return B2AuxiliaryFailure(pid, failure_code, 52001, 0, 0,
                                  GetLastError());
    if (!RestoreB2LoadConfigSlots(pid, process.Get(), failure_code))
        return false;
    for (std::size_t index = 0; index != states.size(); ++index) {
        const auto& patch = kirkware::auxiliary_patches[index];
        auto& state = states[index];
        state.rva = patch.image_rva;
        if (!ReadRemote(process.Get(), kImageBase + state.rva,
                        state.source))
            return B2AuxiliaryFailure(
                pid, failure_code, 52101 + static_cast<int>(index),
                index + 1, state.rva, GetLastError());
        if (!ReadRemote(process.Get(), kBootstrapB2Base + state.rva,
                        state.previous))
            return B2AuxiliaryFailure(
                pid, failure_code, 52201 + static_cast<int>(index),
                index + 1, state.rva, GetLastError());
        if (!state.source)
            return B2AuxiliaryFailure(
                pid, failure_code, 52301 + static_cast<int>(index),
                index + 1, state.rva);
        if (patch.kind == kirkware::AuxiliaryKind::module_base) {
            std::uint16_t signature = 0;
            if (!ReadRemote(process.Get(), state.source, signature))
                return B2AuxiliaryFailure(
                    pid, failure_code, 52401 + static_cast<int>(index),
                    index + 1, state.rva, GetLastError());
            if (signature != IMAGE_DOS_SIGNATURE)
                return B2AuxiliaryFailure(
                    pid, failure_code, 52501 + static_cast<int>(index),
                    index + 1, state.rva, signature);
        } else if (!SpanHasProtection(process.Get(), state.source, 1, true)) {
            return B2AuxiliaryFailure(
                pid, failure_code, 52601 + static_cast<int>(index),
                index + 1, state.rva, state.source);
        }
    }
    for (std::size_t index = 0; index != states.size(); ++index) {
        auto& state = states[index];
        if (state.previous == state.source)
            continue;
        if (!WriteRemoteQword(process.Get(),
                              kBootstrapB2Base + state.rva,
                              state.source)) {
            for (std::size_t rollback = index; rollback != 0; --rollback) {
                auto& prior = states[rollback - 1];
                if (prior.changed)
                    WriteRemoteQword(process.Get(),
                                     kBootstrapB2Base + prior.rva,
                                     prior.previous);
            }
            return B2AuxiliaryFailure(
                pid, failure_code, 52701 + static_cast<int>(index),
                index + 1, state.rva, GetLastError());
        }
        state.changed = true;
    }
    for (const auto& state : states) {
        std::uint64_t observed = 0;
        const std::size_t index =
            static_cast<std::size_t>(&state - states.data());
        if (!ReadRemote(process.Get(), kBootstrapB2Base + state.rva,
                        observed))
            return B2AuxiliaryFailure(
                pid, failure_code, 52801 + static_cast<int>(index),
                index + 1, state.rva, GetLastError());
        if (observed != state.source)
            return B2AuxiliaryFailure(
                pid, failure_code, 52901 + static_cast<int>(index),
                index + 1, state.rva, observed);
    }
    return true;
}

constexpr std::array<std::uint64_t,
                     kirkware::kExpectedGuardedImportCount>
    kGuardedImportRvas{{
        0x58F760, 0x58F770, 0x58F928, 0x58F950,
        0x58F968, 0x58F980, 0x58F990, 0x58F9A8,
        0x58F9B0, 0x58F9B8, 0x58F9C0, 0x58F9C8,
        0x58F9D0, 0x58FA18, 0x58FA20, 0x58FA28,
    }};

bool CaptureActiveNetworkGuard(
    HANDLE process,
    std::vector<kirkware::GuardedImportSlot>& slots) {
    slots.clear();
    slots.reserve(kGuardedImportRvas.size());
    for (const std::uint64_t rva : kGuardedImportRvas) {
        std::uint64_t target = 0;
        if (!ReadRemote(process, kImageBase + rva, target) || !target ||
            !SpanHasProtection(process, target, 1, true))
            return false;
        slots.push_back({rva, target});
    }
    return kirkware::verify_guarded_import_slots(
        process, kImageBase, slots);
}

bool VerifyB2NetworkGuard(DWORD pid) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return false;
    std::vector<kirkware::GuardedImportSlot> slots;
    return CaptureActiveNetworkGuard(process.Get(), slots) &&
           kirkware::verify_guarded_import_slots(
               process.Get(), kBootstrapB2Base, slots);
}

bool MirrorB2NetworkGuard(DWORD pid) {
    struct State {
        std::uint64_t rva = 0;
        std::uint64_t target = 0;
        std::uint64_t previous = 0;
        bool changed = false;
    };
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                                   PROCESS_VM_OPERATION |
                                   PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return false;
    std::vector<kirkware::GuardedImportSlot> active_slots;
    if (!CaptureActiveNetworkGuard(process.Get(), active_slots))
        return false;
    std::array<State, kGuardedImportRvas.size()> states{};
    for (std::size_t index = 0; index != states.size(); ++index) {
        states[index].rva = active_slots[index].image_rva;
        states[index].target = active_slots[index].target;
        if (!ReadRemote(process.Get(),
                        kBootstrapB2Base + states[index].rva,
                        states[index].previous))
            return false;
    }
    const auto rollback = [&]() {
        for (std::size_t index = states.size(); index != 0; --index) {
            auto& state = states[index - 1];
            if (state.changed)
                WriteRemoteQword(process.Get(),
                                 kBootstrapB2Base + state.rva,
                                 state.previous);
        }
    };
    for (auto& state : states) {
        if (state.previous == state.target)
            continue;
        if (!WriteRemoteQword(process.Get(),
                              kBootstrapB2Base + state.rva,
                              state.target)) {
            rollback();
            return false;
        }
        state.changed = true;
    }
    if (!kirkware::seal_network_guard_iat_page(
            process.Get(), kBootstrapB2Base) ||
        !kirkware::verify_guarded_import_slots(
            process.Get(), kBootstrapB2Base, active_slots)) {
        rollback();
        return false;
    }
    return true;
}

bool RegionReadable(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (protection & 0xffu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool RegionExecutable(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (protection & 0xffu) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool SpanHasProtection(HANDLE process, std::uint64_t address,
                       std::size_t size, bool executable) {
    if (!address || !size || address > UINT64_MAX - size)
        return false;
    std::uint64_t cursor = address;
    const std::uint64_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQueryEx(process, reinterpret_cast<const void*>(cursor),
                            &information, sizeof(information)))
            return false;
        const auto region =
            reinterpret_cast<std::uint64_t>(information.BaseAddress);
        if (information.State != MEM_COMMIT || region > cursor ||
            information.RegionSize > UINT64_MAX - region)
            return false;
        if (executable ? !RegionExecutable(information.Protect)
                       : !RegionReadable(information.Protect))
            return false;
        const std::uint64_t region_end = region + information.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = region_end;
    }
    return true;
}

bool VerifyPatchReadback(DWORD pid) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return false;
    constexpr std::array<std::uint8_t, 5> network{
        0xE8, 0x1B, 0x2A, 0x3C, 0x00};
    constexpr std::array<std::uint8_t, 6> late_auth_original{
        0x0F, 0x84, 0xB0, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 2> freshness{0x90, 0x90};
    std::array<std::uint8_t, 7> scoped_marker{};
    std::array<std::uint8_t, 7> scoped_phase{};
    std::array<std::uint8_t, 6> scoped_decision{};
    std::array<std::uint8_t, 6> scoped_accumulator{};
    std::array<std::uint8_t, 10> scoped_e9_source{};
    std::array<std::uint8_t, 10> scoped_gate2_source{};
    std::array<std::uint8_t, kirkware::kScopedEarlyStubSize>
        scoped_stub{};
    std::array<std::uint8_t,
               kirkware::kWatermarkDefaultEntryOriginal.size()>
        watermark_default{};
    std::array<std::uint8_t,
               kirkware::kRendererStateVmHandoffOriginal.size()>
        renderer_state_exit{};
    if (!kirkware::BuildScopedEarlyAuthPatch(
            scoped_marker, scoped_phase, scoped_decision,
            scoped_accumulator, scoped_e9_source, scoped_gate2_source,
            scoped_stub) ||
        !kirkware::BuildWatermarkDefaultEntryPatch(
            watermark_default) ||
        !kirkware::BuildRendererStateVmExitPatch(
            renderer_state_exit))
        return false;
    std::array<std::uint8_t, network.size()> network_actual{};
    std::array<std::uint8_t, scoped_marker.size()>
        scoped_marker_actual{};
    std::array<std::uint8_t, scoped_phase.size()>
        scoped_phase_actual{};
    std::array<std::uint8_t, scoped_decision.size()>
        scoped_decision_actual{};
    std::array<std::uint8_t, scoped_accumulator.size()>
        scoped_accumulator_actual{};
    std::array<std::uint8_t, scoped_e9_source.size()>
        scoped_e9_source_actual{};
    std::array<std::uint8_t, scoped_gate2_source.size()>
        scoped_gate2_source_actual{};
    std::array<std::uint8_t, scoped_stub.size()> scoped_stub_actual{};
    std::array<std::uint8_t, late_auth_original.size()>
        late_auth_actual{};
    std::array<std::uint8_t, freshness.size()> freshness_actual{};
    std::array<std::uint8_t, watermark_default.size()>
        watermark_default_actual{};
    std::array<std::uint8_t, renderer_state_exit.size()>
        renderer_state_exit_actual{};
    std::array<std::uint8_t,
               kirkware::kRendererStateNativeExitOriginal.size()>
        renderer_state_native_exit_actual{};
    return ReadRemoteBytes(process.Get(), kImageBase + 0xE8150,
                           network_actual.data(), network_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedEarlyMarkerEntryRva,
               scoped_marker_actual.data(), scoped_marker_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedEarlyPhaseEntryRva,
               scoped_phase_actual.data(), scoped_phase_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedEarlyDecisionEntryRva,
               scoped_decision_actual.data(),
               scoped_decision_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedAccumulatorEntryRva,
               scoped_accumulator_actual.data(),
               scoped_accumulator_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedE9SourceEntryRva,
               scoped_e9_source_actual.data(),
               scoped_e9_source_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedGate2SourceEntryRva,
               scoped_gate2_source_actual.data(),
               scoped_gate2_source_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kScopedEarlyCaveRva,
               scoped_stub_actual.data(), scoped_stub_actual.size()) &&
           ReadRemoteBytes(process.Get(), kImageBase + 0xBC3F03,
                           late_auth_actual.data(),
                           late_auth_actual.size()) &&
           ReadRemoteBytes(process.Get(), kImageBase + 0xDEF726,
                           freshness_actual.data(), freshness_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kWatermarkDefaultEntryRva,
               watermark_default_actual.data(),
               watermark_default_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kRendererStateVmHandoffRva,
               renderer_state_exit_actual.data(),
               renderer_state_exit_actual.size()) &&
           ReadRemoteBytes(
               process.Get(),
               kImageBase + kirkware::kRendererStateNativeExitRva,
               renderer_state_native_exit_actual.data(),
               renderer_state_native_exit_actual.size()) &&
           network_actual == network &&
           scoped_marker_actual == scoped_marker &&
           scoped_phase_actual == scoped_phase &&
           scoped_decision_actual == scoped_decision &&
           scoped_accumulator_actual == scoped_accumulator &&
           scoped_e9_source_actual == scoped_e9_source &&
           scoped_gate2_source_actual == scoped_gate2_source &&
           scoped_stub_actual == scoped_stub &&
           late_auth_actual == late_auth_original &&
           freshness_actual == freshness &&
           watermark_default_actual == watermark_default &&
           renderer_state_exit_actual == renderer_state_exit &&
           renderer_state_native_exit_actual ==
               kirkware::kRendererStateNativeExitOriginal;
}

bool ProtectedBoundaryReady(HANDLE process) {
    std::uint32_t guard = 0;
    std::uint64_t sentinel = 0;
    std::array<std::uint64_t, 2> links{};
    return ReadRemote(process, kImageBase + 0xB6ECDC, guard) &&
           guard != 0 && guard != UINT32_MAX &&
           (guard & 0x80000000u) != 0 &&
           ReadRemote(process, kImageBase + 0xB6E978, sentinel) &&
           sentinel != 0 && (sentinel & 7u) == 0 &&
           SpanHasProtection(process, sentinel, sizeof(links), false) &&
           ReadRemote(process, sentinel, links) && links[0] == sentinel &&
           links[1] == sentinel;
}

bool WaitForProtectedBoundary(
    DWORD pid, const std::shared_ptr<std::atomic<int>>& entry_result) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION |
                                   SYNCHRONIZE,
                               FALSE, pid));
    if (!process)
        return false;
    const ULONGLONG deadline = GetTickCount64() + 45000;
    do {
        if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
            return false;
        const int result = entry_result->load(std::memory_order_acquire);
        if (ProtectedBoundaryReady(process.Get()))
            return true;
        if (result != kProtectedEntryPending && result != 0)
            return false;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

bool LazyTupleReady(HANDLE process) {
    std::array<std::uint8_t, 0x30> bytes{};
    if (!ReadRemote(process, kImageBase + 0xB6ED30, bytes))
        return false;
    std::uint32_t guard = 0;
    std::uint64_t first = 0;
    std::uint64_t first_count = 0;
    std::uint64_t second = 0;
    std::uint64_t second_count = 0;
    std::memcpy(&guard, bytes.data(), sizeof(guard));
    std::memcpy(&first, bytes.data() + 0x10, sizeof(first));
    std::memcpy(&first_count, bytes.data() + 0x18, sizeof(first_count));
    std::memcpy(&second, bytes.data() + 0x20, sizeof(second));
    std::memcpy(&second_count, bytes.data() + 0x28, sizeof(second_count));
    return guard != 0 && guard != UINT32_MAX &&
           (guard & 0x80000000u) != 0 && first != 0 && second != 0 &&
           (first & 7u) == 0 && (second & 7u) == 0 &&
           first_count == 1 && second_count == 1 &&
           SpanHasProtection(process, first, 0x18, false) &&
           SpanHasProtection(process, second, 0x18, false);
}

struct RendererSnapshot {
    std::uint32_t hook_flag = 0;
    std::uint64_t manager = 0;
    std::uint64_t entry80 = 0;
    std::uint64_t entry150 = 0;
    std::uint64_t saved960 = 0;
    std::uint64_t saved970 = 0;
    std::uint64_t wndproc = 0;
    std::uint8_t flag5ac = 0;
    std::uint8_t flag870 = 0;
    std::uint8_t scoped_auth_used = 0;
    bool read = false;
};

RendererSnapshot CaptureRenderer(HANDLE process) {
    RendererSnapshot value;
    value.read =
        ReadRemote(process, kImageBase + 0xB6BB48, value.hook_flag) &&
        ReadRemote(process, kImageBase + 0xB6BB40, value.manager) &&
        ReadRemote(process, kImageBase + 0x80A960, value.saved960) &&
        ReadRemote(process, kImageBase + 0x80A970, value.saved970) &&
        ReadRemote(process, kImageBase + 0x80A9A0, value.wndproc) &&
        ReadRemote(process, kImageBase + 0x7EA5AC, value.flag5ac) &&
        ReadRemote(process, kImageBase + 0xB6C870, value.flag870) &&
        ReadRemote(process,
                   kImageBase + kirkware::kScopedEarlyCaveRva +
                       kirkware::kScopedEarlyUsedOffset,
                   value.scoped_auth_used);
    if (value.read && value.manager &&
        SpanHasProtection(process, value.manager, 0x158, false)) {
        value.read = ReadRemote(process, value.manager + 0x80,
                                value.entry80) &&
                     ReadRemote(process, value.manager + 0x150,
                                value.entry150);
    }
    return value;
}

bool WndProcReady(HANDLE process, std::uint64_t address) {
    if (!address)
        return false;
    if (SpanHasProtection(process, address, 1, true))
        return true;
    const auto upper = static_cast<std::uint32_t>(address >> 32);
    const auto lower = static_cast<std::uint32_t>(address);
    return (upper == 0 || upper == UINT32_MAX) &&
           (lower & 0xffff0000u) == 0xffff0000u;
}

bool RendererCoreReady(HANDLE process, const RendererSnapshot& value) {
    return value.read && value.hook_flag == 1 && value.manager != 0 &&
           SpanHasProtection(process, value.manager, 0x158, false) &&
           SpanHasProtection(process, value.entry80, 1, true) &&
           SpanHasProtection(process, value.entry150, 1, true) &&
           SpanHasProtection(process, value.saved960, 1, true) &&
           SpanHasProtection(process, value.saved970, 1, true) &&
           WndProcReady(process, value.wndproc) &&
           LazyTupleReady(process);
}

bool WaitForRenderer(
    DWORD pid, bool final_state, DWORD timeout,
    const std::shared_ptr<std::atomic<int>>& entry_result) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION |
                                   SYNCHRONIZE,
                               FALSE, pid));
    if (!process)
        return false;
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
            return false;
        const auto state = CaptureRenderer(process.Get());
        const int worker_result =
            entry_result->load(std::memory_order_acquire);
        if (worker_result != kProtectedEntryPending)
            return false;
        if (RendererCoreReady(process.Get(), state) &&
            (!final_state || (state.flag5ac == 1 && state.flag870 == 1)))
            return true;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    return false;
}

bool RendererReadyWhileParked(DWORD pid, int worker_result) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION |
                                   SYNCHRONIZE,
                               FALSE, pid));
    if (!process ||
        WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
        return false;
    const auto state = CaptureRenderer(process.Get());
    static_cast<void>(worker_result);
    return RendererCoreReady(process.Get(), state) &&
           state.flag5ac == 1 && state.flag870 == 1;
}

bool SetPayloadReady(DWORD pid) {
    Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                                   PROCESS_VM_OPERATION |
                                   PROCESS_QUERY_INFORMATION,
                               FALSE, pid));
    if (!process)
        return false;
    constexpr std::uint64_t address = kImageBase + 0x7D4A64;
    std::uint8_t before = 0;
    if (!ReadRemote(process.Get(), address, before) || before != 0)
        return false;
    const std::uint8_t ready = 1;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process.Get(), reinterpret_cast<void*>(address),
                            &ready, sizeof(ready), &written) ||
        written != sizeof(ready))
        return false;
    std::uint8_t after = 0;
    return ReadRemote(process.Get(), address, after) && after == ready;
}

KirkwareNativeResult Run(HINSTANCE instance,
                              std::wstring_view username_wide,
                              KirkwareReadyCallback ready_callback,
                              void* ready_context,
                              KirkwarePhaseCallback phase_callback,
                              void* phase_context,
                              bool clean_traces,
                              bool run_garrys_mod) {
    const auto set_phase = [phase_callback, phase_context](
                               KirkwareNativePhase phase) {
        if (phase_callback)
            phase_callback(phase_context, phase);
    };
    set_phase(clean_traces ? KirkwareNativePhase::CleaningTraces
                           : KirkwareNativePhase::WaitingForGame);
    const auto early_exit = [](int code, const char* stage) {
        return KirkwareNativeResult{code, stage, 0};
    };
    g_run_garrys_mod_requested.store(
        run_garrys_mod, std::memory_order_release);
    Workspace::CleanupStale();
    if (clean_traces) {
        if (!kirkware::clean_traces::Run())
            return early_exit(70, "clean_traces");
        set_phase(KirkwareNativePhase::WaitingForGame);
    }
    const std::string username = WideToUtf8(username_wide);
    if (!UsernameValid(username))
        return early_exit(10, "username");
    kirkware::gmod_main_menu_gate::Target game_target;
    constexpr DWORD game_wait_timeout_ms = 120000;
    const auto game_status =
        kirkware::gmod_main_menu_gate::Wait(game_target,
                                                 game_wait_timeout_ms);
    if (game_status != kirkware::gmod_main_menu_gate::Status::Ready)
        return early_exit(110 + static_cast<int>(game_status), "game_ready");
    const DWORD pid = game_target.process_id;
    const auto run_exit = [pid](int code, const char* stage) {
        return KirkwareNativeResult{code, stage, pid};
    };
    if (!EnsureConfigDirectory())
        return run_exit(11, "config_directory");
    if (!NormalizeLegacyPlaintextConfigs())
        return run_exit(55, "config_repair");
    Handle target_access(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            SYNCHRONIZE,
        FALSE, pid));
    if (!target_access)
        return run_exit(GetLastError() == ERROR_ACCESS_DENIED ? 118 : 119,
                        "target_access");
    auto workspace = std::make_shared<Workspace>();
    if (!workspace->Create())
        return run_exit(12, "workspace");
    const std::array<std::pair<int, const std::string*>, 5> resources{{
        {kActiveImageResource, &workspace->Active()},
        {kBootstrapB1Resource, &workspace->B1()},
        {kBssResource, &workspace->Bss()},
        {kBssMaskResource, &workspace->Mask()},
        {kLocaleResource, &workspace->Locale()}}};
    for (const auto& resource : resources) {
        if (!WriteResource(instance, resource.first, *resource.second))
            return run_exit(13, "resource_write");
    }
    Handle b2_cache_guard;
    if (!EnsureCachedResource(instance, kBootstrapB2Resource,
                              workspace->B2(), b2_cache_guard))
        return run_exit(13, "resource_write");
    const auto scrub_workspace = [&workspace]() {
        return DeleteWorkspaceFile(workspace->Handoff(), true) &&
               DeleteWorkspaceFile(workspace->Envelope(), true);
    };
    const std::string pid_text = std::to_string(pid);
    const auto revalidate_status =
        kirkware::gmod_main_menu_gate::Validate(game_target, 4, 100);
    if (revalidate_status !=
        kirkware::gmod_main_menu_gate::Status::Ready)
        return run_exit(190 + static_cast<int>(revalidate_status),
                        "main_menu_revalidate");
    if (Invoke(kirkware_handoff_entry,
               {"handoff", workspace->Handoff(), username}) != 0)
        return run_exit(22, "handoff");
    std::uint32_t handoff_unix_time = 0;
    if (!ReadHandoffUnixTime(workspace->Handoff(), handoff_unix_time))
        return run_exit(36, "handoff_time_read");
    if (Invoke(kirkware_envelope_entry,
               {"envelope", workspace->Active(), workspace->Handoff(),
                workspace->Envelope()}) != 0)
        return run_exit(23, "envelope");
    if (InvokeWide(kirkware_bootstrap_entry,
                   {L"bootstrap", std::to_wstring(pid),
                     AnsiToWide(workspace->B1())}) != 0)
        return run_exit(20, "bootstrap_b1");
    if (InvokeWide(kirkware_bootstrap_entry,
                   {L"bootstrap", std::to_wstring(pid),
                     AnsiToWide(workspace->B2())}) != 0)
        return run_exit(21, "bootstrap_b2");
    set_phase(KirkwareNativePhase::PleaseWait);
    const int swap_result = Invoke(
        kirkware_swap_entry,
        {"swap", pid_text, workspace->Active(), workspace->Handoff(), ".",
         workspace->Bss(), workspace->Mask(), "bootstrap-b1.dll"});
    if (swap_result != 0)
        return run_exit(2400 + swap_result, "swap");
    if (!VerifyPatchReadback(pid))
        return run_exit(25, "patch_readback");
    if (!MirrorB2NetworkGuard(pid))
        return run_exit(54, "b2_network_guard");
    if (Invoke(kirkware_context_entry,
               {"context", pid_text, "1E5DCC00000", workspace->Envelope(),
                "envelope"}) != 0)
        return run_exit(27, "context");
    if (Invoke(kirkware_lua_resolver_entry,
               {"lua-resolver", pid_text}) != 0)
        return run_exit(28, "lua_resolver");
    if (Invoke(kirkware_worker_idle_entry,
               {"worker-idle", pid_text}) != 0)
        return run_exit(26, "worker_idle");
    auto lifecycle = std::make_shared<ProtectedLifecycle>();
    if (!lifecycle->Valid())
        return run_exit(29, "barrier_events");
    const std::string barrier_argument = BuildBarrierArgument(
        lifecycle->ready.Get(), lifecycle->release.Get(),
        lifecycle->abort.Get(), lifecycle->success.Get());
    if (barrier_argument.empty())
        return run_exit(29, "barrier_argument");
    auto protected_result =
        std::make_shared<std::atomic<int>>(kProtectedEntryPending);
    auto protected_worker =
        [workspace, lifecycle, pid_text, protected_result,
         barrier_argument, handoff_unix_time]() {
            int result = 36;
            std::lock_guard<std::mutex> lock(ProtectedEnvironmentMutex());
            ScopedEnvironmentOverride fixed_time(
                "KIRKWARE_FIXED_UNIX", std::to_string(handoff_unix_time));
            if (fixed_time.Active()) {
                try {
                    result = Invoke(
                        kirkware_veh_entry,
                        {"veh", pid_text, "0x1E5DCC00000", "1",
                         workspace->Locale(), workspace->Envelope(),
                         kProtectedEntryArgumentMode,
                         barrier_argument});
                } catch (...) {
                    result = 38;
                }
                if (!fixed_time.Restore())
                    result = 37;
            }
            protected_result->store(result, std::memory_order_release);
            SetEvent(lifecycle->done.Get());
        };
    std::thread protected_thread;
    try {
        protected_thread = std::thread(std::move(protected_worker));
    } catch (...) {
        return run_exit(29, "protected_thread_create");
    }
    const auto finish_worker = [&](bool release) {
        HANDLE decision = release ? lifecycle->release.Get()
                                  : lifecycle->abort.Get();
        bool signalled = SetEvent(decision) != FALSE;
        if (!signalled) {
            SetEvent(lifecycle->abort.Get());
        }
        HANDLE native_thread = reinterpret_cast<HANDLE>(
            protected_thread.native_handle());
        HANDLE cleanup_waits[] = {lifecycle->done.Get(), native_thread};
        DWORD wait = WaitForMultipleObjects(
            2, cleanup_waits, FALSE, release ? 250 : 30000);
        const bool completed =
            wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1;
        if (completed && protected_thread.joinable())
            protected_thread.join();
        else if (protected_thread.joinable())
            protected_thread.detach();
        const bool scrubbed = !completed || scrub_workspace();
        const int result =
            protected_result->load(std::memory_order_acquire);
        if (!signalled || !scrubbed)
            return false;
        if (!release)
            return completed;
        return (completed && result == 0) || wait == WAIT_TIMEOUT;
    };
    if (!WaitForProtectedBoundary(pid, protected_result)) {
        finish_worker(false);
        return run_exit(30, "protected_boundary");
    }
    const int first_guard = Invoke(kirkware_steam_offline_guard_entry,
                                   {"steam-offline-guard", pid_text});
    if (first_guard != 0) {
        finish_worker(false);
        return run_exit(5300 + first_guard, "steam_offline_guard");
    }
    if (Invoke(kirkware_locale_entry,
               {"locale", pid_text, "0x1E5DCC00000",
                workspace->Locale()}) != 0) {
        finish_worker(false);
        return run_exit(31, "locale");
    }
    ConfigBoolRegistryState config_before{};
    if (!ReadCompatibleConfigBoolRegistry(pid, kImageBase,
                                          config_before)) {
        finish_worker(false);
        return run_exit(44, "config_registry_before");
    }
    if (config_before.count == 0) {
        if (Invoke(kirkware_carrier_entry,
                   {"carrier", pid_text, "0x1E5DCF40590"}) != 0) {
            finish_worker(false);
            return run_exit(45, "config_defaults");
        }
    }
    ConfigBoolRegistryState config_after{};
    if (!ReadCompatibleConfigBoolRegistry(pid, kImageBase, config_after) ||
        config_after.count == 0 ||
        (config_before.count == 0 &&
         (config_after.count != kConfigDefaultBoolCount ||
          config_after.prewarm_key_count != 0))) {
        finish_worker(false);
        return run_exit(46, "config_registry_after");
    }
    ConfigBoolRegistryState b2_config_before{};
    if (!ReadCompatibleConfigBoolRegistry(pid, kBootstrapB2Base,
                                          b2_config_before)) {
        finish_worker(false);
        return run_exit(49, "b2_config_registry_before");
    }
    int b2_auxiliary_import_error = 52;
    if (!MirrorB2AuxiliaryImports(pid, &b2_auxiliary_import_error)) {
        finish_worker(false);
        return run_exit(52, "b2_auxiliary_imports");
    }
    if (!VerifyB2NetworkGuard(pid)) {
        finish_worker(false);
        return run_exit(54, "b2_network_guard_reverify");
    }
    std::uint32_t b2_crt_state = 0;
    std::uint32_t b2_crt_references = 0;
    if (!ReadRemoteU32(pid, kBootstrapB2Base + 0x7E41D0,
                       b2_crt_state) ||
        !ReadRemoteU32(pid, kBootstrapB2Base + 0x7E4218,
                       b2_crt_references)) {
        finish_worker(false);
        return run_exit(50, "b2_crt_state_before");
    }
    if (b2_crt_state == 0) {
        if (b2_crt_references != 0 ||
            Invoke(kirkware_carrier_entry,
                   {"carrier", pid_text, "0x1E5DFDDC61C",
                    "0x1E5DF940000", "0x0"}) != 0) {
            finish_worker(false);
            return run_exit(50, "b2_crt_initialize");
        }
    } else if (b2_crt_state != 2 || b2_crt_references != 1) {
        finish_worker(false);
        return run_exit(50, "b2_crt_state_before_invalid");
    }
    if (!ReadRemoteU32(pid, kBootstrapB2Base + 0x7E41D0,
                       b2_crt_state) ||
        !ReadRemoteU32(pid, kBootstrapB2Base + 0x7E4218,
                       b2_crt_references) ||
        b2_crt_state != 2 || b2_crt_references != 1) {
        finish_worker(false);
        return run_exit(50, "b2_crt_state_after");
    }
    if (b2_config_before.count == 0 &&
        Invoke(kirkware_carrier_entry,
               {"carrier", pid_text, "0x1E5DFC80590"}) != 0) {
        finish_worker(false);
        return run_exit(50, "b2_config_defaults");
    }
    ConfigBoolRegistryState b2_config_after{};
    if (!ReadCompatibleConfigBoolRegistry(pid, kBootstrapB2Base,
                                          b2_config_after) ||
        b2_config_after.count == 0 ||
        (b2_config_before.count == 0 &&
         (b2_config_after.count != kConfigDefaultBoolCount ||
          b2_config_after.prewarm_key_count != 0))) {
        finish_worker(false);
        return run_exit(51, "b2_config_registry_after");
    }
    set_phase(KirkwareNativePhase::Injecting);
    if (Invoke(kirkware_interfaces_entry,
               {"interfaces", pid_text}) != 0) {
        finish_worker(false);
        return run_exit(32, "interfaces");
    }
    if (Invoke(kirkware_game_hooks_entry,
                {"game_hooks", pid_text}) != 0) {
        finish_worker(false);
        return run_exit(47, "game_hooks");
    }
    if (Invoke(kirkware_carrier_entry,
               {"carrier", pid_text, "0x1E5DCF18B60"}) != 0) {
        finish_worker(false);
        return run_exit(33, "carrier");
    }
    if (!SetPayloadReady(pid)) {
        finish_worker(false);
        return run_exit(34, "payload_ready");
    }
    if (!WaitForRenderer(pid, true, 60000, protected_result)) {
        finish_worker(false);
        return run_exit(35, "renderer_active");
    }
    const int second_guard = Invoke(kirkware_steam_offline_guard_entry,
                                    {"steam-offline-guard", pid_text});
    if (second_guard != 0) {
        finish_worker(false);
        return run_exit(5400 + second_guard,
                        "steam_offline_guard_reverify");
    }
    HANDLE capture_acknowledgement[] = {
        lifecycle->ready.Get(), lifecycle->done.Get()};
    const DWORD capture_acknowledged = WaitForMultipleObjects(
        2, capture_acknowledgement, FALSE, 5000);
    if (capture_acknowledged != WAIT_OBJECT_0) {
        finish_worker(false);
        return run_exit(43, "return_capture_ready");
    }
    const int parked_result =
        protected_result->load(std::memory_order_acquire);
    if (parked_result != kProtectedEntryPending) {
        finish_worker(false);
        return run_exit(40, "worker_not_parked");
    }
    if (!RendererReadyWhileParked(pid, parked_result)) {
        finish_worker(false);
        return run_exit(41, "renderer_parked");
    }
    const bool workspace_cleaned = workspace->Cleanup();
    if (!workspace_cleaned) {
        finish_worker(false);
        return run_exit(53, "workspace_cleanup");
    }
    if (ready_callback)
        ready_callback(ready_context);
    if (protected_thread.joinable())
        protected_thread.detach();
    return run_exit(0, "success");
}

}

KirkwareNativeResult RunKirkwareNativeChain(
    HINSTANCE instance,
    std::wstring_view username,
    KirkwareReadyCallback ready_callback,
    void* ready_context,
    KirkwarePhaseCallback phase_callback,
    void* phase_context,
    bool clean_traces,
    bool run_garrys_mod) {
    return Run(instance, username, ready_callback, ready_context,
               phase_callback, phase_context, clean_traces,
               run_garrys_mod);
}
