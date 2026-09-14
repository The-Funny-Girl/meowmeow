#include "gmod_main_menu_gate.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace kirkware::gmod_main_menu_gate
{
namespace
{
class Handle
{
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_)
    {
        other.value_ = nullptr;
    }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            if (value_ && value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    ~Handle()
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    HANDLE Get() const { return value_; }
    explicit operator bool() const
    {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_ = nullptr;
};

struct ModuleInfo
{
    std::uintptr_t base = 0;
    DWORD size = 0;
    std::wstring path;
};

struct ParsedImage
{
    const IMAGE_NT_HEADERS64* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    WORD section_count = 0;
};

struct Match
{
    std::uint32_t rva = 0;
    std::uint32_t raw_offset = 0;
};

struct Candidate
{
    DWORD process_id = 0;
    bool runtime_ready = false;
    bool window_ready = false;
    std::uintptr_t window = 0;
    std::uint64_t creation_time = 0;
};

struct WindowSearch
{
    DWORD process_id = 0;
    std::vector<HWND> matches;
};

bool AddFits(std::size_t left, std::size_t right, std::size_t limit)
{
    return left <= limit && right <= limit - left;
}

bool ReadExact(HANDLE process, std::uintptr_t address, void* output,
               std::size_t size)
{
    SIZE_T completed = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             output, size, &completed) != FALSE &&
           completed == size;
}

bool ProcessCreationTime(DWORD process_id, std::uint64_t& value)
{
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                               FALSE, process_id));
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!process ||
        !GetProcessTimes(process.Get(), &created, &exited, &kernel, &user) ||
        WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
        return false;
    value = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
            created.dwLowDateTime;
    return value != 0;
}

BOOL CALLBACK CollectWindow(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search.process_id || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) != 0)
        return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right - client.left < 320 ||
        client.bottom - client.top < 200)
        return TRUE;
    DWORD_PTR response = 0;
    if (!SendMessageTimeoutW(window, WM_NULL, 0, 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000,
                             &response))
        return TRUE;
    search.matches.push_back(window);
    return TRUE;
}

bool FindWindow(DWORD process_id, std::uintptr_t& window)
{
    WindowSearch search;
    search.process_id = process_id;
    if (!EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&search)) ||
        search.matches.size() != 1)
        return false;
    window = reinterpret_cast<std::uintptr_t>(search.matches.front());
    return window != 0;
}

bool ReadRuntime(DWORD process_id, ModuleInfo* engine)
{
    Handle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (!snapshot)
        return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.Get(), &module))
        return false;
    std::array<unsigned, 3> counts{};
    ModuleInfo found_engine;
    do
    {
        if (_wcsicmp(module.szModule, L"engine.dll") == 0)
        {
            ++counts[0];
            found_engine.base =
                reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            found_engine.size = module.modBaseSize;
            found_engine.path = module.szExePath;
        }
        else if (_wcsicmp(module.szModule, L"client.dll") == 0)
            ++counts[1];
        else if (_wcsicmp(module.szModule, L"lua_shared.dll") == 0)
            ++counts[2];
    } while (Module32NextW(snapshot.Get(), &module));
    if (!std::all_of(counts.begin(), counts.end(),
                     [](unsigned count) { return count == 1; }))
        return false;
    if (engine)
        *engine = std::move(found_engine);
    return true;
}

Status SelectTarget(Target& target)
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return Status::NotFound;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
        return Status::NotFound;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (!Process32FirstW(snapshot.Get(), &process))
        return Status::NotFound;
    std::vector<Candidate> candidates;
    bool saw_gmod = false;
    bool saw_runtime = false;
    do
    {
        if (_wcsicmp(process.szExeFile, L"gmod.exe") != 0)
            continue;
        DWORD candidate_session = 0;
        if (!ProcessIdToSessionId(process.th32ProcessID, &candidate_session) ||
            candidate_session != session)
            continue;
        saw_gmod = true;
        Candidate candidate;
        candidate.process_id = process.th32ProcessID;
        candidate.runtime_ready = ReadRuntime(candidate.process_id, nullptr);
        saw_runtime = saw_runtime || candidate.runtime_ready;
        if (candidate.runtime_ready)
        {
            candidate.window_ready =
                FindWindow(candidate.process_id, candidate.window);
            if (!ProcessCreationTime(candidate.process_id,
                                     candidate.creation_time))
                candidate.window_ready = false;
        }
        if (candidate.runtime_ready && candidate.window_ready)
            candidates.push_back(candidate);
    } while (Process32NextW(snapshot.Get(), &process));
    if (candidates.size() > 1)
        return Status::Ambiguous;
    if (candidates.empty())
        return !saw_gmod ? Status::NotFound
                         : saw_runtime ? Status::WindowUnavailable
                                       : Status::RuntimeUnavailable;
    target.process_id = candidates.front().process_id;
    target.creation_time = candidates.front().creation_time;
    target.window = candidates.front().window;
    return Status::Ready;
}

bool LoadFile(const std::wstring& path, std::vector<std::uint8_t>& output)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr));
    if (!file)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart <= 0 ||
        size.QuadPart > 512ll * 1024ll * 1024ll)
        return false;
    output.assign(static_cast<std::size_t>(size.QuadPart), 0);
    std::size_t offset = 0;
    while (offset < output.size())
    {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            output.size() - offset, 16u * 1024u * 1024u));
        DWORD completed = 0;
        if (!ReadFile(file.Get(), output.data() + offset, requested,
                      &completed, nullptr) ||
            completed != requested)
            return false;
        offset += completed;
    }
    return true;
}

bool ParseImage(const std::vector<std::uint8_t>& file, ParsedImage& output)
{
    if (file.size() < sizeof(IMAGE_DOS_HEADER))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return false;
    const std::size_t nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (!AddFits(nt_offset, sizeof(IMAGE_NT_HEADERS64), file.size()))
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        file.data() + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader !=
            sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96 ||
        nt->OptionalHeader.SizeOfImage == 0 ||
        nt->OptionalHeader.SizeOfHeaders == 0 ||
        nt->OptionalHeader.SizeOfHeaders > file.size())
        return false;
    const std::size_t section_offset =
        nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!AddFits(section_offset, section_bytes, file.size()) ||
        section_offset + section_bytes > nt->OptionalHeader.SizeOfHeaders)
        return false;
    output.nt = nt;
    output.sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        file.data() + section_offset);
    output.section_count = nt->FileHeader.NumberOfSections;
    return true;
}

bool PatternMatches(const std::uint8_t* data, std::uint8_t immediate,
                    std::uint8_t setcc)
{
    return data[0] == 0x83 && data[1] == 0x3D && data[6] == immediate &&
           data[7] == 0x0F && data[8] == setcc && data[9] == 0xC0 &&
           data[10] == 0xC3;
}

std::vector<Match> FindPattern(const std::vector<std::uint8_t>& file,
                               const ParsedImage& image,
                               std::uint8_t immediate,
                               std::uint8_t setcc)
{
    std::vector<Match> matches;
    constexpr std::size_t size = 11;
    for (WORD index = 0; index < image.section_count; ++index)
    {
        const auto& section = image.sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.SizeOfRawData < size)
            continue;
        const std::size_t raw = section.PointerToRawData;
        const std::size_t raw_size = section.SizeOfRawData;
        if (!AddFits(raw, raw_size, file.size()))
            continue;
        for (std::size_t offset = 0; offset + size <= raw_size; ++offset)
        {
            if (!PatternMatches(file.data() + raw + offset, immediate, setcc))
                continue;
            const std::uint64_t rva =
                static_cast<std::uint64_t>(section.VirtualAddress) + offset;
            if (rva > std::numeric_limits<std::uint32_t>::max() ||
                rva + size > image.nt->OptionalHeader.SizeOfImage)
                continue;
            matches.push_back(
                {static_cast<std::uint32_t>(rva),
                 static_cast<std::uint32_t>(raw + offset)});
        }
    }
    return matches;
}

bool ResolveTarget(const std::vector<std::uint8_t>& file,
                   const Match& match, std::uint32_t image_size,
                   std::uint32_t& output)
{
    if (!AddFits(match.raw_offset + 2, sizeof(std::int32_t), file.size()))
        return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, file.data() + match.raw_offset + 2,
                sizeof(displacement));
    const std::int64_t target = static_cast<std::int64_t>(match.rva) + 7 +
                                static_cast<std::int64_t>(displacement);
    if (target < 0 ||
        target + static_cast<std::int64_t>(sizeof(std::int32_t)) >
            image_size)
        return false;
    output = static_cast<std::uint32_t>(target);
    return true;
}

bool TargetSectionValid(const ParsedImage& image, std::uint32_t target)
{
    unsigned containing = 0;
    for (WORD index = 0; index < image.section_count; ++index)
    {
        const auto& section = image.sections[index];
        const std::uint64_t begin = section.VirtualAddress;
        const std::uint64_t span =
            std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        const std::uint64_t end = begin + span;
        if (target < begin ||
            static_cast<std::uint64_t>(target) + sizeof(std::int32_t) > end)
            continue;
        ++containing;
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
            return false;
    }
    return containing == 1;
}

bool RemoteIdentityValid(HANDLE process, const ModuleInfo& module,
                         const std::vector<std::uint8_t>& file,
                         const ParsedImage& image)
{
    const auto* file_dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    IMAGE_DOS_HEADER dos{};
    if (!ReadExact(process, module.base, &dos, sizeof(dos)) ||
        dos.e_magic != file_dos->e_magic || dos.e_lfanew < 0 ||
        dos.e_lfanew != file_dos->e_lfanew)
        return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadExact(process,
                   module.base + static_cast<std::uintptr_t>(dos.e_lfanew),
                   &nt, sizeof(nt)) ||
        nt.Signature != image.nt->Signature ||
        std::memcmp(&nt.FileHeader, &image.nt->FileHeader,
                    sizeof(nt.FileHeader)) != 0)
        return false;
    const std::uint64_t mapped_base = nt.OptionalHeader.ImageBase;
    if (mapped_base != image.nt->OptionalHeader.ImageBase &&
        mapped_base != module.base)
        return false;
    IMAGE_OPTIONAL_HEADER64 remote_optional = nt.OptionalHeader;
    IMAGE_OPTIONAL_HEADER64 file_optional = image.nt->OptionalHeader;
    remote_optional.ImageBase = 0;
    file_optional.ImageBase = 0;
    if (std::memcmp(&remote_optional, &file_optional,
                    sizeof(remote_optional)) != 0 ||
        module.size != image.nt->OptionalHeader.SizeOfImage)
        return false;
    const std::size_t section_offset =
        static_cast<std::size_t>(dos.e_lfanew) +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t section_bytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!AddFits(section_offset, section_bytes, file.size()) ||
        section_offset + section_bytes > nt.OptionalHeader.SizeOfHeaders)
        return false;
    std::vector<std::uint8_t> remote_sections(section_bytes);
    return ReadExact(process, module.base + section_offset,
                     remote_sections.data(), remote_sections.size()) &&
           std::memcmp(remote_sections.data(), file.data() + section_offset,
                       section_bytes) == 0;
}

bool RemotePatternValid(HANDLE process, const ModuleInfo& module,
                        const std::vector<std::uint8_t>& file,
                        const Match& match)
{
    std::array<std::uint8_t, 11> remote{};
    return ReadExact(process, module.base + match.rva, remote.data(),
                     remote.size()) &&
           std::equal(remote.begin(), remote.end(),
                      file.begin() + match.raw_offset);
}

bool RemoteTargetValid(HANDLE process, const ModuleInfo& module,
                       std::uint32_t target)
{
    MEMORY_BASIC_INFORMATION memory{};
    const std::uintptr_t address = module.base + target;
    if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                       &memory, sizeof(memory)) != sizeof(memory))
        return false;
    const std::uintptr_t region =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t allocation =
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    const DWORD protection = memory.Protect & 0xFFu;
    const bool readable = protection == PAGE_READONLY ||
                          protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY;
    return memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
           allocation == module.base &&
           (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 && readable &&
           address >= region && address + sizeof(std::int32_t) >= address &&
           address + sizeof(std::int32_t) <= region + memory.RegionSize;
}

Status ResolveState(const Target& target, Handle& process,
                    ModuleInfo& module, std::uint32_t& state_rva)
{
    std::uint64_t creation_time = 0;
    std::uintptr_t window = 0;
    if (!ProcessCreationTime(target.process_id, creation_time) ||
        creation_time != target.creation_time ||
        !FindWindow(target.process_id, window) || window != target.window)
        return Status::IdentityChanged;
    if (!ReadRuntime(target.process_id, &module) || module.base == 0 ||
        module.size == 0 || module.path.empty())
        return Status::RuntimeUnavailable;
    std::vector<std::uint8_t> file;
    ParsedImage image;
    if (!LoadFile(module.path, file) || !ParseImage(file, image))
        return Status::StateUnavailable;
    process = Handle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                     SYNCHRONIZE,
                                 FALSE, target.process_id));
    if (!process || !RemoteIdentityValid(process.Get(), module, file, image))
        return Status::StateUnavailable;
    const auto in_game = FindPattern(file, image, 0x06, 0x94);
    const auto connected = FindPattern(file, image, 0x02, 0x9D);
    std::uint32_t first_target = 0;
    std::uint32_t second_target = 0;
    if (in_game.size() != 1 || connected.size() != 1 ||
        !ResolveTarget(file, in_game.front(),
                       image.nt->OptionalHeader.SizeOfImage, first_target) ||
        !ResolveTarget(file, connected.front(),
                       image.nt->OptionalHeader.SizeOfImage, second_target) ||
        first_target != second_target ||
        !TargetSectionValid(image, first_target) ||
        !RemotePatternValid(process.Get(), module, file, in_game.front()) ||
        !RemotePatternValid(process.Get(), module, file, connected.front()) ||
        !RemoteTargetValid(process.Get(), module, first_target))
        return Status::StateUnavailable;
    state_rva = first_target;
    return Status::Ready;
}
}

Status Validate(const Target& target, unsigned samples, DWORD interval_ms)
{
    if (target.process_id == 0 || target.creation_time == 0 ||
        target.window == 0 || samples == 0)
        return Status::IdentityChanged;
    Handle process;
    ModuleInfo module;
    std::uint32_t state_rva = 0;
    const Status resolved = ResolveState(target, process, module, state_rva);
    if (resolved != Status::Ready)
        return resolved;
    for (unsigned sample = 0; sample < samples; ++sample)
    {
        if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0)
            return Status::IdentityChanged;
        std::int32_t signon = -1;
        if (!ReadExact(process.Get(), module.base + state_rva, &signon,
                       sizeof(signon)))
            return Status::StateUnavailable;
        if (signon != 0)
            return Status::NotMainMenu;
        std::uintptr_t window = 0;
        if (!FindWindow(target.process_id, window) || window != target.window)
            return Status::WindowUnavailable;
        if (sample + 1 < samples)
            Sleep(interval_ms);
    }
    std::uint64_t creation_time = 0;
    if (!ProcessCreationTime(target.process_id, creation_time) ||
        creation_time != target.creation_time)
        return Status::IdentityChanged;
    return Status::Ready;
}

Status Wait(Target& target, DWORD timeout_ms)
{
    const ULONGLONG start = GetTickCount64();
    do
    {
        Target selected;
        Status status = SelectTarget(selected);
        if (status == Status::Ready)
        {
            const Status validated = Validate(selected, 4, 100);
            if (validated == Status::Ready)
            {
                target = selected;
                return Status::Ready;
            }
            if (validated == Status::NotMainMenu)
                return validated;
            status = validated;
        }
        else if (status == Status::Ambiguous)
            return status;
        if (GetTickCount64() - start >= timeout_ms)
            return status;
        Sleep(250);
    } while (true);
}
}
