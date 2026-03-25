#pragma once

/// @file core/util.hpp
/// @brief Shared utility functions used across the core.

#include <cstdint>
#include <filesystem>
#include <string>

namespace gn {

/// @brief Convert binary data to lowercase hex string.
/// @param data  Pointer to byte buffer.
/// @param len   Number of bytes to convert.
/// @return Hex-encoded string (2 chars per byte).
std::string bytes_to_hex(const uint8_t* data, size_t len);

/// @brief Expand leading "~/" to $HOME (or $USERPROFILE on Windows).
/// @param p  Path that may start with "~/".
/// @return Expanded absolute path.
std::filesystem::path expand_home(const std::filesystem::path& p);

} // namespace gn
