#pragma once
/// @file include/version.hpp
/// @brief GoodNet framework version constants.

#define GOODNET_VERSION_MAJOR 0
#define GOODNET_VERSION_MINOR 1
#define GOODNET_VERSION_PATCH 0
#define GOODNET_VERSION_TAG   "alpha"

#define GOODNET_VERSION_STRING "0.1.0-alpha"

/// Packed core version for wire protocol: (major<<16)|(minor<<8)|patch.
/// Used in CoreMeta handshake payload so peers can detect version mismatches.
#define GOODNET_VERSION_PACKED \
    ((static_cast<uint32_t>(GOODNET_VERSION_MAJOR) << 16) | \
     (static_cast<uint32_t>(GOODNET_VERSION_MINOR) <<  8) | \
     (static_cast<uint32_t>(GOODNET_VERSION_PATCH)))

namespace gn {

/// @brief Runtime version string ("0.1.0-alpha").
inline constexpr const char* version() noexcept { return GOODNET_VERSION_STRING; }

/// @brief Runtime packed version for wire protocol comparison.
inline constexpr uint32_t version_packed() noexcept { return GOODNET_VERSION_PACKED; }

} // namespace gn
