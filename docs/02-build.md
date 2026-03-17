# 02 — Сборка и запуск

## Nix Flakes (рекомендуется)

```bash
# Dev окружение: cmake, ninja, gdb, ccache, cmake-format, jq, lcov
nix develop

# Полная сборка (core + плагины + bundle):
nix build

# Отдельные пакеты:
nix build .#core       # только libgoodnet_core
nix build .#plugins    # все плагины (linkFarm)
nix build .#docker     # Docker-образ (OCI tar)
```

### Dev shell алиасы

```bash
cfg   → cmake -B build       -DCMAKE_BUILD_TYPE=Release -G Ninja
b     → cmake --build build
brun  → cmake --build build && ./build/goodnet

cfgd  → cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -G Ninja
bd    → cmake --build build/debug
bdrun → cmake --build build/debug && ./build/debug/goodnet
```

Первый запуск: `cfg && b` (Release) или `cfgd && bd` (Debug). Дальше — только `b` / `bd` (инкрементальная сборка).

### Nix run (без алиасов)

```bash
nix run              # Debug build + запуск goodnet
nix run .#test       # Debug build + unit tests
nix run .#coverage   # Coverage report (lcov → HTML)
```

---

## CMake (ручная сборка)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja -DBUILD_TESTING=ON
cmake --build build -j$(nproc)

# Запуск тестов:
./build/bin/unit_tests

# Запуск бинарника:
./build/goodnet --listen 25565
```

### CMake Presets

В проекте есть `CMakePresets.json` с готовыми конфигурациями:

```bash
# Linux (Ninja)
cmake --preset linux-release
cmake --build --preset linux-release

# Windows (vcpkg + Visual Studio)
cmake --preset windows-msvc

# Windows (vcpkg + Ninja)
cmake --preset windows-ninja
```

Список всех пресетов: `cmake --list-presets`

### Windows (vcpkg)

Зависимости разрешаются автоматически через манифест `vcpkg.json`:

```powershell
# Клонировать vcpkg (если ещё нет)
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
set VCPKG_ROOT=%cd%\vcpkg

# Сборка через пресет
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

Или вручную:

```powershell
cmake -B build -G "Visual Studio 17 2022" ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DBUILD_TESTING=ON
cmake --build build --config Release
```

### Поддержка платформ

| Возможность | Linux | macOS | Windows |
|---|---|---|---|
| TCP коннектор | да | да | да |
| ICE коннектор | да | нет | нет |
| Динамические плагины (.so/.dylib/.dll) | да | да | да |
| Статические плагины | да | да | да |
| Docker-образ | да | нет | нет |

> ICE коннектор использует `<sys/epoll.h>` — только Linux.
> TCP коннектор полностью кроссплатформенный.

### Кросс-компиляция

Нативная кросс-компиляция из Linux в Windows **непрактична** с текущим стеком зависимостей (Boost, libsodium, spdlog). Рекомендуемые подходы:

1. **Windows-машина или VM** — нативная сборка с MSVC + vcpkg
2. **GitHub Actions CI** — раннер `windows-latest` с кэшированием vcpkg
3. **WSL2** — собирает Linux-бинарники, не Windows

mingw-w64 теоретически возможен, но ненадёжен для Boost/spdlog/FTXUI.

### Требования к компилятору

- C++23: GCC 15+, Clang 18+, MSVC 17.12+
- CMake 3.22+
- Ninja (рекомендуется) или Make

### Зависимости

| Библиотека | Версия | Используется |
|---|---|---|
| Boost (system, filesystem, program_options) | ≥1.81 | Asio, CLI args |
| libsodium | ≥1.0.18 | Всё крипто (Ed25519, X25519, XSalsa20, BLAKE2b, SHA-256) |
| zstd | ≥1.5 | Сжатие пакетов >512 байт |
| spdlog + fmt | ≥1.12 | Логирование |
| nlohmann_json | ≥3.11 | Config, манифесты плагинов |
| GTest | ≥1.14 | Тесты |

### CMake targets

| Target | Тип | Описание |
|---|---|---|
| `goodnet_sdk` | INTERFACE | Заголовки SDK для плагинов |
| `goodnet_core` | SHARED / STATIC | `libgoodnet_core.so` — всё ядро (`-DGOODNET_STATIC_CORE=ON` → STATIC) |
| `goodnet` | EXECUTABLE | Benchmark / server бинарник |
| `unit_tests` | EXECUTABLE | GTest suite |
| `mock_handler` | SHARED | Mock handler для тестов |
| `mock_connector` | SHARED | Mock connector для тестов |

Почему `goodnet_core` — SHARED по умолчанию: одна копия статических переменных (Logger singleton, `std::call_once` флаг). При STATIC каждый динамический плагин получил бы свою копию → несколько синглтонов → логи в разные файлы. Используйте `GOODNET_STATIC_CORE=ON` только совместно с `GOODNET_STATIC_PLUGINS=ON` (единый бинарник без динамических плагинов).

### Флаги сборки

```cmake
target_compile_definitions(goodnet_core
    PUBLIC  SPDLOG_COMPILED_LIB
    PRIVATE GOODNET_PROJECT_ROOT="..."
)
set_target_properties(goodnet_core PROPERTIES
    ENABLE_EXPORTS ON   # -rdynamic: плагины видят символы ядра
    SOVERSION   0
    VERSION     0.1.0
)
```

### PCH (precompiled headers)

```cmake
# cmake/pch.cmake: apply_pch(target)
apply_pch(goodnet_core)
apply_pch(goodnet)
```

Отключение: `-DGOODNET_DISABLE_PCH=ON` (используется в Nix-сборке).

### ccache

В dev shell настроен автоматически:

```bash
export CCACHE_DIR="$HOME/.cache/ccache"
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Повторная сборка — ускорение в 10–100x.

---

## CLI флаги

```
GoodNet Benchmark:
  -h [ --help ]            Show help
  -t [ --target ] URI      Target URI (tcp://IP:PORT)
  -l [ --listen ] PORT     Server listen port
  -j [ --threads ] N       IO+worker threads (0=auto)
  -n [ --count ] N         Packet limit (0=unlimited)
  -s [ --size ] KB         Packet size in KB (default: 64)
  --hz HZ                  Dashboard refresh rate (default: 2.0)
  --no-color               Disable ANSI colors
  --exit-after SEC         Exit after N seconds (для CI; 0=disabled)
  --exit-code              Structured exit codes: 0=ok, 1=crypto error
  --ice-upgrade            Upgrade to ICE/DTLS after TCP handshake
  -c [ --config ] PATH     Path to JSON config file
```

### Примеры запуска

```bash
# Сервер (слушает TCP на порту 25565):
./result/bin/goodnet --listen 25565

# Клиент (подключается и шлёт 64KB пакеты, 4 потока):
./result/bin/goodnet --target tcp://192.168.1.2:25565 --size 64 -j 4

# CI: 10 секунд, потом выход с кодом:
./result/bin/goodnet --target tcp://127.0.0.1:25565 --exit-after 10 --exit-code

# ICE/NAT traversal:
./result/bin/goodnet --target tcp://peer:25565 --ice-upgrade --config ci/config-ice.json
```

---

## Переменные окружения

| Переменная | Назначение | Пример |
|---|---|---|
| `GOODNET_PLUGINS_DIR` | Путь к директории плагинов | `/opt/goodnet/plugins` |
| `GOODNET_STUN_SERVER` | STUN сервер для ICE | `stun.l.google.com` |
| `GOODNET_STUN_PORT` | STUN порт для ICE | `19302` |
| `GOODNET_SDK_PATH` | Путь к SDK (dev shell) | Устанавливается автоматически |
| `CCACHE_DIR` | Кэш ccache | `~/.cache/ccache` |

Приоритет конфигурации ICE: JSON config > env vars > default (`stun.l.google.com:19302`).

---

## CoreConfig (программная конфигурация)

```cpp
struct CoreConfig {
    struct {
        fs::path dir            = "~/.goodnet";  // хранилище ключей
        fs::path ssh_key_path;                    // путь к SSH-ключу (опционально)
        bool     use_machine_id = true;           // hardware-bound device_key
    } identity;

    struct {
        std::vector<fs::path> dirs;               // директории плагинов
        bool auto_load = true;                    // автозагрузка при старте
    } plugins;

    struct {
        std::string listen_address = "0.0.0.0";
        uint16_t    listen_port    = 25565;
        int         io_threads     = 0;           // 0 = hardware_concurrency()
    } network;

    struct {
        std::string level     = "info";           // trace/debug/info/warn/error
        std::string file;                         // путь к лог-файлу
        size_t      max_size  = 10 * 1024 * 1024; // ротация: 10 MB
        int         max_files = 5;
    } logging;

    fs::path config_file;                         // JSON config файл
};
```

---

## Добавление нового плагина

### CMake

```cmake
# plugins/handlers/my_handler/CMakeLists.txt
add_library(my_handler SHARED my_handler.cpp)
target_link_libraries(my_handler PRIVATE goodnet_core)
```

### Nix (для дистрибуции)

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

`buildPlugin.nix` автоматически вычисляет SHA-256 и создаёт манифест `my_handler.so.json`.

---

## Coverage

```bash
nix run .#coverage
# или вручную:
cmake -B build/coverage -DCMAKE_BUILD_TYPE=Debug -G Ninja \
      -DBUILD_TESTING=ON -DGOODNET_COVERAGE=ON
cmake --build build/coverage
lcov --zerocounters --directory build/coverage
./build/coverage/bin/unit_tests
lcov --capture --directory build/coverage --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

HTML-отчёт: `build/coverage/coverage_html/index.html`

---

*← [01 — Обзор](01-overview.md) · [03 — Core API →](03-core-api.md)*
