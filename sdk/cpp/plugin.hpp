#pragma once

#include "../handler.h"
#include "../connector.h"
#include "../plugin.h"
#include "logger.hpp"

#include <memory>

// ─── Видимость символов ───────────────────────────────────────────────────────
// GN_EXPORT: только handler_init / connector_init видны снаружи .so.
// Всё остальное скрыто через -fvisibility=hidden (add_plugin макрос в helper.cmake).
// Это предотвращает конфликты имён между плагинами при RTLD_LOCAL.

#if defined(_WIN32)
    #define GN_EXPORT __declspec(dllexport)
#else
    #define GN_EXPORT __attribute__((visibility("default")))
#endif

// ─── sync_plugin_context ─────────────────────────────────────────────────────
//
// Плагины загружены с RTLD_LOCAL: у каждого своя копия статических переменных,
// включая Logger::get_instance(). По умолчанию instance = nullptr → SIGSEGV.
//
// Ядро передаёт api->internal_logger = Logger::get().get()
// (сырой указатель на свой spdlog::logger, живущий в Meyers singleton ядра).
//
// sync_plugin_context оборачивает его в shared_ptr<no-op deleter>:
//   • "Нет владения" — плагин не удаляет объект при destroy
//   • При dlclose(): no-op deleter → нет двойного удаления
//   • Ядро управляет временем жизни через Logger::shutdown()
//
// Потокобезопасность: вызывается строго до instance.init(api), однопоточно.

inline void sync_plugin_context(host_api_t* api) {
    if (!api || !api->internal_logger) return;
    Logger::set_external_logger(
        std::shared_ptr<spdlog::logger>(
            static_cast<spdlog::logger*>(api->internal_logger),
            [](spdlog::logger*) noexcept {}  // no-op: объект принадлежит ядру
        )
    );
}

// ─── HANDLER_PLUGIN ───────────────────────────────────────────────────────────
//
// Генерирует точку входа: extern "C" handler_t* handler_init(host_api_t*)
//
// Жизненный цикл static instance:
//   • Создаётся при первом вызове handler_init (ленивая инициализация).
//   • Разрушается при dlclose() → ~ClassName() → no more LOG_* (logger уже null).
//   • Явный shutdown через handler->shutdown() вызывается в PluginManager ПЕРЕД dlclose.
//
// Использование:
//   class MyHandler : public gn::IHandler { ... };
//   HANDLER_PLUGIN(MyHandler)

#define HANDLER_PLUGIN(ClassName)                                 \
extern "C" GN_EXPORT handler_t* handler_init(host_api_t* api) {  \
    if (!api) return nullptr;                                     \
    sync_plugin_context(api);                                     \
    static ClassName instance;                                    \
    instance.init(api);                                           \
    return instance.to_c_handler();                               \
}

// ─── CONNECTOR_PLUGIN ─────────────────────────────────────────────────────────

#define CONNECTOR_PLUGIN(ClassName)                                    \
extern "C" GN_EXPORT connector_ops_t* connector_init(host_api_t* api) { \
    if (!api) return nullptr;                                          \
    sync_plugin_context(api);                                          \
    static ClassName instance;                                         \
    instance.init(api);                                                \
    return instance.to_c_ops();                                        \
}
