# 13 — Сборка

---

## Nix Flakes (рекомендуется)

```bash
# Dev окружение: cmake, ninja, gdb, ccache, cmake-format, jq
nix develop

# Алиасы внутри dev shell:
cfg   → cmake -B build       -DCMAKE_BUILD_TYPE=Release -G Ninja
b     → cmake --build build
brun  → cmake --build build && ./build/goodnet

cfgd  → cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -G Ninja
bd    → cmake --build build/debug
bdrun → cmake --build build/debug && ./build/debug/goodnet

# Первый запуск:
cfg && b          # Release
cfgd && bd        # Debug
# Последующие: только b / bd (инкрементальная сборка)
```

### Nix пакеты

```bash
nix build              # default: core + все плагины + bundle
nix build .#debug      # Debug с coverage
nix build .#core       # только libgoodnet_core
nix build .#plugins    # все плагины
nix build .#bundle     # bundle плагинов
```

---

## Зависимости

| Библиотека | Используется |
|---|---|
| Boost (system, filesystem, program_options) | IO, signals, CLI args |
| libsodium | Всё крипто |
| zstd | Сжатие пакетов >512 байт |
| spdlog + fmt | Логирование |
| nlohmann_json | Config, манифесты плагинов |
| **ftxui** | **CLI TUI (терминальный UI)** |
| GTest | Тесты |

---

## CMake targets

| Target | Тип | Описание |
|---|---|---|
| `goodnet_sdk` | INTERFACE | Заголовки SDK для плагинов |
| `goodnet_core` | SHARED | `libgoodnet_core.so` — всё ядро |
| `goodnet` | EXECUTABLE | Бенчмарк / основной бинарник |
| `unit_tests` | EXECUTABLE | GTest suite |
| `mock_handler` | SHARED | Mock handler для тестов |
| `mock_connector` | SHARED | Mock connector для тестов |

### Почему SHARED для core?

```
goodnet_core SHARED  →  одна копия статических переменных
                         Logger singleton (std::call_once флаг)
                         Всегда shared между ядром и плагинами
```

STATIC core: каждый плагин получил бы свою копию Logger → несколько синглтонов → логи в разные файлы.

---

## Структура src/ и cli/

```
src/
├── main.cpp          # CLI entry point (argparse + gn::Core + benchmark)
├── core.cpp          # gn::Core::Impl — тяжёлые зависимости здесь
├── capi.cpp          # C API: gn_core_create/destroy/run/stop/send
├── config.cpp
├── logger.cpp        # spdlog включается ТОЛЬКО здесь
└── signals.cpp       # EventSignalBase::Impl реализация

cli/                  # TUI (ftxui), подключается только к исполняемому файлу
├── app.cpp           # Точка входа TUI-приложения
├── cli.hpp           # Типы, команды, интерфейс
├── commands.cpp      # Обработчики: connect, disconnect, send, stats, plugins
└── views.cpp         # Виджеты: таблица соединений, лог событий, stats panel
```

`core.cpp` содержит единственный `#include <boost/asio.hpp>`. Все остальные TU подключают только `core.hpp` (Pimpl) или SDK-заголовки. Это принципиально: время компиляции downstream-кода не зависит от Boost.

---

## PCH (precompiled headers)

```cmake
# cmake/pch.cmake: apply_pch(target)
# Убирает повторную компиляцию <string>, <vector>, <memory> и т.д.
apply_pch(goodnet_core)
apply_pch(goodnet)
```

---

## Добавление плагина

### Через CMake

```cmake
# plugins/handlers/my_handler/CMakeLists.txt
add_library(my_handler SHARED my_handler.cpp)
target_link_libraries(my_handler PRIVATE goodnet_core)
```

### Через Nix (для дистрибуции)

```nix
# plugins/handlers/my_handler/default.nix
{ pkgs, mkCppPlugin, goodnetSdk }:
mkCppPlugin {
  name    = "my_handler";
  src     = ./.;
  sdk     = goodnetSdk;
  sources = [ "my_handler.cpp" ];
}
```

`buildPlugin.nix` автоматически вычисляет SHA-256 и создаёт `my_handler.so.json`.

---

## ccache

```bash
export CCACHE_DIR="$HOME/.cache/ccache"
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
```

В dev shell настроен автоматически. Повторная сборка — ускорение в 10–100x.

---

## Флаги сборки ядра

```cmake
target_compile_definitions(goodnet_core
    PUBLIC  SPDLOG_COMPILED_LIB          # использовать скомпилированный spdlog
    PRIVATE GOODNET_PROJECT_ROOT="..."   # для обрезки путей в логах
)

set_target_properties(goodnet_core PROPERTIES
    ENABLE_EXPORTS ON   # -rdynamic: плагины видят символы ядра
    SOVERSION   0
    VERSION     0.1.0
)
```

---

*← [12 — C++ SDK](12-sdk-cpp.md) · [14 — Тестирование →](14-testing.md)*