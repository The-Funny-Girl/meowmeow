#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace kirkware::platform {

bool EnsurePrivateDirectory(const std::filesystem::path& path,
                            std::string* error = nullptr);
bool AtomicWriteText(const std::filesystem::path& path,
                     std::string_view text,
                     std::string* error = nullptr);
bool ReadTextFile(const std::filesystem::path& path,
                  std::size_t maximum_size,
                  std::string& text,
                  std::string* error = nullptr);
bool ProbeWritableDirectory(const std::filesystem::path& path,
                            std::string* error = nullptr);

} // namespace kirkware::platform
