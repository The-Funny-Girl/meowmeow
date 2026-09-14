#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kirkware::config_sanitizer {

enum class Result : std::uint8_t {
    Unchanged,
    Rewritten,
    Quarantined,
    Rejected,
    IoError,
};

struct BufferReport {
    Result result = Result::Rejected;
    std::vector<std::uint8_t> encoded;
    std::size_t retained = 0;
    std::size_t removed = 0;
    std::size_t adjusted = 0;
};

BufferReport SanitizeBytes(const std::vector<std::uint8_t>& input);
Result SanitizeFile(const std::wstring& path);

}
