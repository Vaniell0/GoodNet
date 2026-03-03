#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <expected>

// ─── Платформенные инклуды ────────────────────────────────────────────────────

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

// ─── Расширение файла плагина для текущей платформы ──────────────────────────

#if defined(_WIN32)
    inline constexpr std::string_view DYNLIB_EXT = ".dll";
#elif defined(__APPLE__)
    inline constexpr std::string_view DYNLIB_EXT = ".dylib";
#else
    inline constexpr std::string_view DYNLIB_EXT = ".so";
#endif

// ─── DynLib ───────────────────────────────────────────────────────────────────
//
// RAII-обёртка над платформенным загрузчиком разделяемых библиотек.
//
//   Linux / macOS : dlopen(RTLD_NOW | RTLD_LOCAL)  dlsym  dlclose
//   Windows       : LoadLibraryW  GetProcAddress  FreeLibrary
//
// Флаги dlopen:
//   RTLD_NOW    — все неразрешённые символы проверяются немедленно.
//                 Ошибка при dlopen(), а не при первом вызове функции.
//   RTLD_LOCAL  — символы плагина видны только внутри него. Плагины изолированы
//                 друг от друга, нет конфликтов имён символов.
//                 Logger не нужен через RTLD_GLOBAL: он передаётся явно через
//                 api->internal_logger (sync_plugin_context в plugin.hpp).

class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }

    DynLib(const DynLib&)            = delete;
    DynLib& operator=(const DynLib&) = delete;

    DynLib(DynLib&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    DynLib& operator=(DynLib&& other) noexcept {
        if (this != &other) {
            close();
            handle_       = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // ── Открытие ─────────────────────────────────────────────────────────────

    [[nodiscard]]
    static std::expected<DynLib, std::string> open(const fs::path& path) {
        DynLib lib;

#if defined(_WIN32)
        lib.handle_ = static_cast<void*>(
            ::LoadLibraryW(path.wstring().c_str())
        );
        if (!lib.handle_)
            return std::unexpected(win32_error());
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

    // ── Получение символа ─────────────────────────────────────────────────────
    //
    // auto fn = lib.symbol<handler_t*(*)(host_api_t*)>("get_handler");

    template<typename Fn>
    [[nodiscard]]
    std::expected<Fn, std::string> symbol(std::string_view name) const {
        if (!handle_)
            return std::unexpected("Library not loaded");

#if defined(_WIN32)
        void* sym = reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(handle_),
                             std::string(name).c_str())
        );
        if (!sym)
            return std::unexpected(win32_error());
#else
        ::dlerror();
        void* sym = ::dlsym(handle_, std::string(name).c_str());
        const char* err = ::dlerror();
        if (err)
            return std::unexpected(std::string(err));
#endif
        return reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(sym));
    }

    // ── Состояние / закрытие ─────────────────────────────────────────────────

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    explicit operator bool()    const noexcept  { return is_open(); }

    // close() безопасен благодаря порядку завершения в main():
    //   manager.unload_all() вызывает shutdown() всех плагинов до dlclose.
    //   После shutdown() on_shutdown() уже отработал — LOG_* не вызываются.
    //   Logger::shutdown() вызывается после unload_all() — logger жив до закрытия.
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
        ::FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, buf, sizeof(buf), nullptr
        );
        return std::string(buf);
    }
#endif
};

} // namespace gn
