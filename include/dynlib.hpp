#pragma once

/// @file core/dynlib.hpp
/// @brief RAII wrapper for platform dynamic library loading.
///
/// Linux/macOS: dlopen(RTLD_NOW | RTLD_LOCAL) / dlsym / dlclose
/// Windows:     LoadLibraryW / GetProcAddress / FreeLibrary
///
/// RTLD_LOCAL isolates plugin symbols — no cross-plugin name conflicts.
/// Logger is passed explicitly via api->internal_logger, not via RTLD_GLOBAL.

#include <string>
#include <string_view>
#include <filesystem>
#include <expected>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace gn {

namespace fs = std::filesystem;

#if defined(_WIN32)
    inline constexpr std::string_view DYNLIB_EXT = ".dll";
#elif defined(__APPLE__)
    inline constexpr std::string_view DYNLIB_EXT = ".dylib";
#else
    inline constexpr std::string_view DYNLIB_EXT = ".so";
#endif

/// @brief Move-only shared library handle.
class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }

    DynLib(const DynLib&)            = delete;
    DynLib& operator=(const DynLib&) = delete;

    DynLib(DynLib&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    DynLib& operator=(DynLib&& other) noexcept {
        if (this != &other) { close(); handle_ = other.handle_; other.handle_ = nullptr; }
        return *this;
    }

    [[nodiscard]]
    static std::expected<DynLib, std::string> open(const fs::path& path) {
        DynLib lib;
#if defined(_WIN32)
        lib.handle_ = static_cast<void*>(::LoadLibraryW(path.wstring().c_str()));
        if (!lib.handle_) return std::unexpected(win32_error());
#else
        ::dlerror();
        lib.handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib.handle_) {
            const char* err = ::dlerror();
            return std::unexpected(err ? std::string(err) : "dlopen failed");
        }
#endif
        return lib;
    }

    /// @brief Lookup exported symbol by name.
    template<typename Fn>
    [[nodiscard]]
    std::expected<Fn, std::string> symbol(std::string_view name) const {
        if (!handle_) return std::unexpected("Library not loaded");
#if defined(_WIN32)
        void* sym = reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(handle_), std::string(name).c_str()));
        if (!sym) return std::unexpected(win32_error());
#else
        ::dlerror();
        void* sym = ::dlsym(handle_, std::string(name).c_str());
        const char* err = ::dlerror();
        if (err) return std::unexpected(std::string(err));
#endif
        return reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(sym));
    }

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    explicit operator bool()    const noexcept  { return is_open(); }

    void close() noexcept {
        if (!handle_) return;
#if defined(_WIN32)
        ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
    }

private:
    void* handle_ = nullptr;

#if defined(_WIN32)
    static std::string win32_error() {
        DWORD code = ::GetLastError();
        char buf[512] = {};
        ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, buf, sizeof(buf), nullptr);
        return std::string(buf);
    }
#endif
};

} // namespace gn
