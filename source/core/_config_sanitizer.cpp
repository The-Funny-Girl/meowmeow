#include "kirkware_config_sanitizer.hpp"
#include "kirkware_config_schema.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kirkware::config_sanitizer {
namespace {

constexpr std::string_view kCipherKey = "51287373";
constexpr std::size_t kMaximumFileSize = 1024 * 1024;
constexpr std::size_t kMaximumLineSize = 4096;
constexpr std::size_t kMaximumKeySize = 256;
constexpr std::size_t kMaximumStringSize = 2048;
constexpr std::array<char, 6> kSections{{'b', 'i', 'f', 'c', 's', 'm'}};
constexpr char kExcludedEntitiesSection = 'e';

struct Entry {
    char section = 0;
    std::string key;
    std::string value;
};

struct NativeKeyCorpus {
    bool available = false;
    std::unordered_map<std::string, char> sections;
};

bool IsSection(char value) {
    return value == kExcludedEntitiesSection ||
           std::find(kSections.begin(), kSections.end(), value) !=
               kSections.end();
}

bool IsKeyCharacter(unsigned char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool IsTextByte(unsigned char value) {
    return value == '\t' || (value >= 0x20 && value <= 0x7e);
}

void ApplyCipher(std::vector<std::uint8_t>& bytes) {
    for (std::size_t index = 0; index != bytes.size(); ++index)
        bytes[index] ^= static_cast<std::uint8_t>(
            kCipherKey[index % kCipherKey.size()]);
}

bool ParseNativeSchema(NativeKeyCorpus& result) {
    const std::string_view schema(detail::kNativeConfigSchema);
    std::size_t offset = 0;
    while (offset < schema.size()) {
        const std::size_t end = schema.find('\n', offset);
        const std::string_view line = schema.substr(
            offset, (end == std::string_view::npos ? schema.size() : end) -
                        offset);
        offset = end == std::string_view::npos ? schema.size() : end + 1;
        if (line.empty())
            continue;
        if (line.size() < 3 || line[1] != ' ' ||
            std::find(kSections.begin(), kSections.end(), line[0]) ==
                kSections.end())
            return false;
        const std::string_view key = line.substr(2);
        if (key.empty() || key.size() > kMaximumKeySize ||
            !std::all_of(key.begin(), key.end(), [](unsigned char value) {
                return IsKeyCharacter(value);
            }))
            return false;
        if (!result.sections.emplace(std::string(key), line[0]).second)
            return false;
    }
    return result.sections.size() == detail::kNativeConfigSchemaEntryCount;
}

const NativeKeyCorpus& NativeKeys() {
    static const NativeKeyCorpus corpus = [] {
        NativeKeyCorpus result;
        if (!ParseNativeSchema(result))
            return result;
        HMODULE module = GetModuleHandleW(nullptr);
        if (!module)
            return result;
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(401),
                                       MAKEINTRESOURCEW(10));
        if (!resource)
            return result;
        HGLOBAL loaded = LoadResource(module, resource);
        const auto* bytes = static_cast<const std::uint8_t*>(
            loaded ? LockResource(loaded) : nullptr);
        const std::size_t size = SizeofResource(module, resource);
        if (!bytes || !size)
            return result;
        std::unordered_set<std::string> witnessed;
        std::size_t offset = 0;
        while (offset < size) {
            if (!IsKeyCharacter(bytes[offset])) {
                ++offset;
                continue;
            }
            const std::size_t begin = offset;
            while (offset < size && IsKeyCharacter(bytes[offset]))
                ++offset;
            if (offset < size && bytes[offset] == 0 && offset > begin &&
                offset - begin <= kMaximumKeySize)
            {
                std::string name(
                    reinterpret_cast<const char*>(bytes + begin),
                    offset - begin);
                if (result.sections.find(name) != result.sections.end())
                    witnessed.emplace(std::move(name));
            }
            if (offset == begin)
                ++offset;
        }
        result.available = witnessed.size() == result.sections.size();
        return result;
    }();
    return corpus;
}

bool IsNativeKey(const Entry& entry, const NativeKeyCorpus& corpus) {
    if (entry.section == kExcludedEntitiesSection)
        return true;
    const auto found = corpus.sections.find(entry.key);
    return found != corpus.sections.end() && found->second == entry.section;
}

bool ParseConfig(const std::vector<std::uint8_t>& bytes,
                 std::vector<Entry>& entries,
                 bool& saw_excluded_entities_section) {
    if (bytes.size() > kMaximumFileSize)
        return false;
    saw_excluded_entities_section = false;
    char section = 0;
    bool saw_section = false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t end = offset;
        while (end < bytes.size() && bytes[end] != '\r' &&
               bytes[end] != '\n') {
            if (!IsTextByte(bytes[end]) || end - offset >= kMaximumLineSize)
                return false;
            ++end;
        }
        std::string line(reinterpret_cast<const char*>(bytes.data() + offset),
                         end - offset);
        if (end == bytes.size()) {
            offset = end;
        } else if (bytes[end] == '\n') {
            offset = end + 1;
        } else if (end + 1 < bytes.size() && bytes[end + 1] == '\n') {
            offset = end + 2;
        } else {
            return false;
        }
        if (line.empty())
            continue;
        if (line == "[excluded_entities]") {
            section = kExcludedEntitiesSection;
            saw_section = true;
            saw_excluded_entities_section = true;
            continue;
        }
        if (line.size() == 3 && line[0] == '[' && line[2] == ']' &&
            IsSection(line[1]) && line[1] != kExcludedEntitiesSection) {
            section = line[1];
            saw_section = true;
            continue;
        }
        if (!section)
            return false;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0 ||
            separator > kMaximumKeySize)
            return false;
        std::string key = line.substr(0, separator);
        if (!std::all_of(key.begin(), key.end(), [](unsigned char value) {
                return IsTextByte(value) && value != '=';
            }))
            return false;
        std::string value = line.substr(separator + 1);
        if ((section == 's' && value.size() > kMaximumStringSize) ||
            (section != 's' && value.size() > kMaximumLineSize))
            return false;
        entries.push_back({section, std::move(key), std::move(value)});
    }
    return saw_section;
}

bool ParseInteger(std::string_view text, std::int32_t& value) {
    if (text.empty())
        return false;
    const char* first = text.data();
    const char* last = first + text.size();
    auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool ParseFloat(std::string_view text, float& value) {
    if (text.empty() || text.size() >= 128)
        return false;
    std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    value = std::strtof(buffer.c_str(), &end);
    return errno != ERANGE && end == buffer.c_str() + buffer.size() &&
           std::isfinite(value);
}

bool ParseColor(std::string_view text) {
    std::size_t offset = 0;
    for (int component = 0; component != 4; ++component) {
        const std::size_t comma = text.find(',', offset);
        const std::size_t end = comma == std::string_view::npos
                                    ? text.size()
                                    : comma;
        std::int32_t value = 0;
        if (!ParseInteger(text.substr(offset, end - offset), value) ||
            value < 0 || value > 255)
            return false;
        if (component != 3) {
            if (comma == std::string_view::npos)
                return false;
            offset = comma + 1;
        } else if (comma != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool ParseMulti(std::string_view text) {
    if (text.empty())
        return true;
    if (text.size() > kMaximumLineSize || text.back() != '|')
        return false;
    std::set<std::int32_t> indices;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t end = text.find('|', offset);
        if (end == std::string_view::npos || end == offset)
            return false;
        const std::string_view item = text.substr(offset, end - offset);
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            colon + 1 >= item.size() || item.find(':', colon + 1) !=
                                               std::string_view::npos)
            return false;
        std::int32_t index = 0;
        std::int32_t enabled = 0;
        if (!ParseInteger(item.substr(0, colon), index) || index < 0 ||
            index > 4096 ||
            !ParseInteger(item.substr(colon + 1), enabled) ||
            (enabled != 0 && enabled != 1) || !indices.insert(index).second)
            return false;
        offset = end + 1;
    }
    return true;
}

bool ValidateValue(const Entry& entry) {
    if (entry.section == kExcludedEntitiesSection)
        return entry.value == "0" || entry.value == "1";
    if (entry.section == 'b')
        return entry.value == "0" || entry.value == "1";
    if (entry.section == 'i') {
        std::int32_t value = 0;
        return ParseInteger(entry.value, value);
    }
    if (entry.section == 'f') {
        float value = 0.0f;
        return ParseFloat(entry.value, value);
    }
    if (entry.section == 'c')
        return ParseColor(entry.value);
    if (entry.section == 's')
        return entry.value.size() <= kMaximumStringSize;
    return entry.section == 'm' && ParseMulti(entry.value);
}

std::vector<std::uint8_t> Serialize(
    const std::vector<Entry>& entries,
    bool include_excluded_entities_section) {
    std::string plain;
    for (char section : kSections) {
        plain.push_back('[');
        plain.push_back(section);
        plain += "]\r\n";
        for (const Entry& entry : entries) {
            if (entry.section != section)
                continue;
            plain += entry.key;
            plain.push_back('=');
            plain += entry.value;
            plain += "\r\n";
        }
    }
    if (include_excluded_entities_section) {
        plain += "[excluded_entities]\r\n";
        for (const Entry& entry : entries) {
            if (entry.section != kExcludedEntitiesSection)
                continue;
            plain += entry.key;
            plain.push_back('=');
            plain += entry.value;
            plain += "\r\n";
        }
    }
    std::vector<std::uint8_t> encoded(plain.begin(), plain.end());
    ApplyCipher(encoded);
    return encoded;
}

bool ReadFileBytes(const std::wstring& path,
                   std::vector<std::uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool success = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                   size.QuadPart <= static_cast<LONGLONG>(kMaximumFileSize);
    if (success) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        success = bytes.empty() ||
                  (ReadFile(file, bytes.data(),
                            static_cast<DWORD>(bytes.size()), &read,
                            nullptr) &&
                   read == bytes.size());
    }
    CloseHandle(file);
    return success;
}

bool WriteFileBytes(const std::wstring& path,
                    const std::vector<std::uint8_t>& bytes) {
    const std::wstring temporary = path + L".kirkware-repair.tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL |
                                  FILE_ATTRIBUTE_TEMPORARY,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool success = WriteFile(file, bytes.data(),
                             static_cast<DWORD>(bytes.size()), &written,
                             nullptr) &&
                   written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (success)
        success = MoveFileExW(temporary.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING |
                                  MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success)
        DeleteFileW(temporary.c_str());
    return success;
}

bool EnsureBackup(const std::wstring& path) {
    const std::wstring backup = path + L".kirkware-backup";
    if (CopyFileW(path.c_str(), backup.c_str(), TRUE))
        return true;
    if (GetLastError() != ERROR_FILE_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesW(backup.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool QuarantineFile(const std::wstring& path) {
    for (std::uint32_t index = 0; index != 10000; ++index) {
        std::wstring quarantine = path + L".kirkware-quarantine";
        if (index)
            quarantine += L"." + std::to_wstring(index);
        if (MoveFileExW(path.c_str(), quarantine.c_str(),
                        MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return false;
    }
    return false;
}

}

BufferReport SanitizeBytes(const std::vector<std::uint8_t>& input) {
    BufferReport report{};
    if (input.empty()) {
        report.result = Result::Unchanged;
        return report;
    }
    if (input.size() > kMaximumFileSize)
        return report;
    const NativeKeyCorpus& corpus = NativeKeys();
    if (!corpus.available) {
        // Payload schema does not list every loader key (e.g. original
        // playerlist_secretservice vs kirkware). Skip rewrite instead of
        // failing inject with config_repair/55.
        report.result = Result::Unchanged;
        return report;
    }
    std::vector<Entry> entries;
    bool encoded = false;
    bool saw_excluded_entities_section = false;
    if (!ParseConfig(input, entries, saw_excluded_entities_section)) {
        std::vector<std::uint8_t> plain = input;
        ApplyCipher(plain);
        entries.clear();
        if (!ParseConfig(plain, entries,
                         saw_excluded_entities_section))
            return report;
        encoded = true;
    }
    std::vector<Entry> retained;
    retained.reserve(entries.size());
    std::unordered_map<std::string, std::size_t> retained_indices;
    retained_indices.reserve(entries.size());
    for (Entry& entry : entries) {
        if (!IsNativeKey(entry, corpus) || !ValidateValue(entry)) {
            ++report.removed;
            continue;
        }
        std::string identity;
        identity.reserve(entry.key.size() + 2);
        identity.push_back(entry.section);
        identity.push_back('\0');
        identity += entry.key;
        const auto existing = retained_indices.find(identity);
        if (existing == retained_indices.end()) {
            retained_indices.emplace(std::move(identity), retained.size());
            retained.push_back(std::move(entry));
        } else {
            retained[existing->second].value = std::move(entry.value);
            ++report.removed;
        }
    }
    report.retained = retained.size();

    const bool repaired = report.removed != 0 || report.adjusted != 0;
    if (!repaired) {
        report.encoded = input;
        if (!encoded)
            ApplyCipher(report.encoded);
        report.result = encoded ? Result::Unchanged : Result::Rewritten;
        return report;
    }

    const bool retained_excluded_entity =
        std::any_of(retained.begin(), retained.end(), [](const Entry& entry) {
            return entry.section == kExcludedEntitiesSection;
        });
    report.encoded = Serialize(
        retained,
        saw_excluded_entities_section || retained_excluded_entity);
    report.result = Result::Rewritten;
    return report;
}

Result SanitizeFile(const std::wstring& path) {
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(path, bytes))
        return Result::IoError;
    BufferReport report = SanitizeBytes(bytes);
    if (report.result == Result::Rejected)
        return QuarantineFile(path) ? Result::Quarantined : Result::IoError;
    if (report.result != Result::Rewritten)
        return report.result;
    if (!EnsureBackup(path))
        return Result::IoError;
    return WriteFileBytes(path, report.encoded) ? Result::Rewritten
                                                : Result::IoError;
}

}
