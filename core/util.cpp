/// @file core/util.cpp

#include "util.hpp"

#include <cstdio>
#include <cstdlib>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char b[3];
        std::snprintf(b, 3, "%02x", data[i]);
        out += b;
    }
    return out;
}

std::filesystem::path expand_home(const std::filesystem::path& p) {
    std::string s = p.string();
    if (!s.starts_with("~/")) return p;
    const char* h = std::getenv("HOME");
#if defined(_WIN32)
    if (!h) h = std::getenv("USERPROFILE");
#endif
    return std::filesystem::path(h ? h : ".") / s.substr(2);
}

} // namespace gn
