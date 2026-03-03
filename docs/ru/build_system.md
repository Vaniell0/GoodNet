# GoodNet — Система сборки

GoodNet использует **Nix Flakes** как верхнеуровневую систему сборки и **CMake + Ninja** как инструмент компиляции C++. Nix обеспечивает воспроизводимость и изолированное управление зависимостями; CMake решает детали компиляции, которые Nix ему делегирует.

---

## Быстрый старт

```bash
# Войти в среду разработки (настраивает PATH, cmake, ninja, ccache, SDK path)
nix develop

# Сконфигурировать и собрать инкрементально (быстро, на уровне файлов)
cfgd && bd          # Debug
cfg  && b           # Release

# Запустить
./build/debug/goodnet
./build/goodnet

# Полная воспроизводимая сборка (вывод в Nix store)
nix build           # Release → ./result/bin/goodnet
nix build .#debug   # Debug   → ./result/bin/goodnet
```

---

## Структура Nix Flake

```
flake.nix
nix/
├── mkCppPlugin.nix    # Собирает один плагин из исходников
└── buildPlugin.nix    # Подписывает плагин, генерирует .so.json манифест
```

### Граф деривации

```
nixpkgs (boost, spdlog, fmt, nlohmann_json, libsodium)
    │
    ▼
goodnet-core          ← libgoodnet_core.so + заголовки + cmake SDK
    │
    ├─► handlers/logger    ← liblogger.so + liblogger.so.json
    ├─► handlers/...
    ├─► connectors/tcp     ← libtcp.so + libtcp.so.json
    └─► connectors/...
         │
         ▼
    pluginsBundle      ← плоская директория: handlers/*.so, connectors/*.so
         │
         ▼
    fullApp            ← bin/goodnet + plugins/ + переменные окружения
```

Каждая деривация адресована по содержимому. Если исходники или зависимости не изменились, Nix мгновенно возвращает закешированный результат из `/nix/store`. При изменении одного плагина пересобирается только он — `goodnet-core` и остальные плагины берутся из кеша.

---

## Именованные пакеты

```bash
nix build               # .#default — полное Release-приложение
nix build .#debug       # Debug с символами и консольным выводом лога
nix build .#core        # только libgoodnet_core.so + SDK
nix build .#plugins     # все плагины (просматриваемое дерево)
nix build .#bundle      # плоская директория плагинов
```

Отдельные плагины:
```bash
nix build .#plugins.handlers.logger
nix build .#plugins.connectors.tcp
```

---

## Деривация `goodnet-core`

```nix
goodnet-core = pkgs.stdenv.mkDerivation {
  pname = "goodnet-core";
  version = "0.1.0-alpha";
  src = ./.;

  nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];

  buildInputs           = with pkgs; [ nlohmann_json libsodium boost ];
  propagatedBuildInputs = with pkgs; [ spdlog fmt ];
  # propagated: плагины находят spdlog/fmt транзитивно через CMAKE_PREFIX_PATH

  cmakeFlags = [
    "-DINSTALL_DEVELOPMENT=ON"    # устанавливает заголовки SDK + GoodNetConfig.cmake
    "-DCMAKE_BUILD_TYPE=Release"
  ];
};
```

`propagatedBuildInputs` — ключевое отличие от обычного `buildInputs`. Когда нижележащая деривация (плагин) имеет `goodnet-core` в своём `buildInputs`, Nix автоматически добавляет `spdlog` и `fmt` в `CMAKE_PREFIX_PATH` этой деривации. Это необходимо, потому что `GoodNetConfig.cmake` вызывает `find_dependency(spdlog)` и `find_dependency(fmt)` — эти пакеты должны быть найдены при запуске CMake плагина.

`nlohmann_json` и `libsodium` **не** распространяются, поскольку являются PRIVATE-зависимостями ядра (используются только в `.cpp`-файлах). Плагинам их находить не нужно.

---

## Пайплайн сборки плагинов: `mkCppPlugin`

`nix/mkCppPlugin.nix` — фабричная функция, вызываемая из `default.nix` каждого плагина:

```nix
# plugins/handlers/logger/default.nix
{ pkgs, mkCppPlugin, goodnetSdk, ... }:

mkCppPlugin {
  name        = "logger";
  type        = "handlers";      # определяет поддиректорию установки
  version     = "1.0.0";
  description = "Записывает входящие пакеты в бинарные бандл-файлы";
  src         = ./.;
  deps        = [];              # дополнительные Nix-пакеты, напр. pkgs.boost
  inherit goodnetSdk;
}
```

Внутри `mkCppPlugin` выполняет два шага:

**Шаг 1 — `rawBuild`:** компилирует плагин через CMake:
```
cmake -DCMAKE_PREFIX_PATH=${goodnetSdk} -DBUILD_SHARED_LIBS=ON ...
```
Это позволяет `find_package(GoodNet REQUIRED)` работать, направляя CMake к установленному SDK.

**Шаг 2 — `buildPlugin`:** берёт скомпилированный `.so`, вычисляет SHA-256 и записывает JSON-манифест:
```json
{
  "meta": { "name": "logger", "type": "handlers", "version": "1.0.0", ... },
  "integrity": { "alg": "sha256", "hash": "abc123..." }
}
```
Файл манифеста называется `liblogger.so.json` (добавляется суффикс, расширение не заменяется). `PluginManager` читает и верифицирует его перед вызовом `dlopen`.

---

## Debug-сборка: `nix build .#debug`

Debug-пакет использует ту же функцию `makeCore` с `buildType = "Debug"`:

```nix
goodnet-core-debug = makeCore { buildType = "Debug"; };
```

Эффекты `CMAKE_BUILD_TYPE=Debug`:
- Нет `-O2`/`-O3` — символы не удаляются, GDB может пошагово выполнять код
- `NDEBUG` не определён → `LOG_DEBUG`, `LOG_TRACE`, `SCOPED_TRACE()` активны
- Консольный синк компилируется (см. `#ifndef NDEBUG` в `logger.cpp`)
- PCH включён (см. ниже) — инкрементальные debug-сборки быстрые

Debug-плагины собираются против `goodnet-core-debug` — состояние `NDEBUG` консистентно.

**Поведение кеша:** `nix build .#debug` полностью кешируется при неизменных исходниках. Повторный запуск без изменения файлов занимает меньше секунды.

**После `nixos-rebuild switch` с включённым `impure-derivations`** раскомментируйте `__impure = true` в `flake.nix` для истинно инкрементальной компиляции на уровне файлов через персистентный CMake-кеш в `$HOME`.

---

## Среда разработки

```bash
nix develop
```

Открывает shell с полной средой сборки. `shellHook` настраивает:

| Переменная | Значение |
|---|---|
| `GOODNET_SDK_PATH` | Путь к установленному SDK в Nix store |
| `CCACHE_DIR` | `~/.cache/ccache` — ccache переиспользует объектные файлы между сессиями |
| `CMAKE_CXX_COMPILER_LAUNCHER` | `ccache` — прозрачно, без изменений в CMake |

Алиасы для быстрой итерации:

| Алиас | Команда | Примечание |
|---|---|---|
| `cfg` | `cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja` | Конфигурация Release |
| `b` | `cmake --build build` | Сборка Release (инкрементально) |
| `brun` | `cmake --build build && ./build/goodnet` | Сборка + запуск |
| `cfgd` | `cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -G Ninja` | Конфигурация Debug |
| `bd` | `cmake --build build/debug` | Сборка Debug (инкрементально, с PCH) |
| `bdrun` | `cmake --build build/debug && ./build/debug/goodnet` | Сборка + запуск |

Первичная настройка:
```bash
cfgd    # сконфигурировать один раз
bd      # сборка (медленно в первый раз)
bdrun   # сборка + запуск
```

При последующих правках одного `.cpp`:
```bash
bd      # пересобираются только изменённые файлы
```

---

## Структура CMake

```
CMakeLists.txt
cmake/
├── pch.cmake              # Настройка Precompiled Headers (только Debug)
├── GoodNetConfig.cmake.in # Шаблон конфига для установленного SDK
└── gen_manifests.cmake    # Таргет: генерация .so.json для ручных сборок
```

### Ключевые CMake-таргеты

| Таргет | Тип | Описание |
|---|---|---|
| `goodnet_sdk` | INTERFACE library | Только заголовки SDK, без скомпилированного кода |
| `goodnet_core` | SHARED library | Основной фреймворк, SOVERSION 0 |
| `goodnet` | Executable | Основное приложение |

### `add_plugin()` (из `helper.cmake`)

Используется в `CMakeLists.txt` каждого плагина. Устанавливает:
- `PREFIX "lib"` → `libmyplugin.so`
- `CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN ON` → `-fvisibility=hidden`
- `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections` → минимальный размер `.so`
- Вызывает `apply_plugin_pch()` при `CMAKE_BUILD_TYPE == Debug`

---

## Precompiled Headers (PCH)

`cmake/pch.cmake` включается из корневого `CMakeLists.txt` и предоставляет две функции:

- `apply_pch(target)` — для `goodnet_core` и исполняемого файла `goodnet`
- `apply_plugin_pch(target)` — вызывается внутри `add_plugin()` для плагинов

Обе являются **no-op в Release**. В Debug вызывают `target_precompile_headers()` с тяжёлыми заголовками:

```
spdlog/spdlog.h          ← экономия ~0.8с на TU
nlohmann/json.hpp        ← экономия ~1.2с на TU
fmt/format.h             ← экономия ~0.3с на TU
STL (string, vector, memory, mutex, filesystem, ...)
```

PCH компилируется один раз на директорию сборки. При повторном `bd` все эти заголовки берутся из `.gch`-файла — пересобираются только изменённые `.cpp`.

---

## Сводка зависимостей

| Пакет | Используется | Распространяется |
|---|---|---|
| `boost` | ядро (сигналы, asio), tcp-плагин | да |
| `spdlog` | логгер ядра | да |
| `fmt` | ядро, плагины | да |
| `nlohmann_json` | парсер конфигурации ядра | нет (PRIVATE) |
| `libsodium` | SHA-256 манифестов плагинов | нет (PRIVATE) |
| `cmake`, `ninja`, `pkg-config` | только сборка (nativeBuildInputs) | нет |
| `makeWrapper` | только обёртка fullApp | нет |
