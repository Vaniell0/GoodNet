# Сборка

Подробности сборки GoodNet: Nix, CMake, опции, Docker CI.

См. также: [Быстрый старт](./quickstart.md) · [Система плагинов](./architecture/plugin-system.md) · **[Build tips →](./recipes/build-tips.md)**

## Nix (рекомендуется)

```bash
nix develop          # shell со всеми зависимостями
nix build            # сборка + тесты
./result/bin/goodnet --help
```

Nix собирает core + три плагина (TCP, ICE, Logger) как `.so`, подписывает их (SHA-256 manifest), упаковывает в `result/`.

**Gotcha**: Nix использует git tree. Новые файлы нужно `git add` перед `nix build` — иначе Nix их не увидит.

## CMake

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
./build/bin/unit_tests
```

### Зависимости

- GCC 14+ или Clang 18+ (C++23)
- boost (asio, filesystem, program_options)
- libsodium
- spdlog
- fmt
- nlohmann_json
- zstd
- GTest (для тестов)

**Установка зависимостей:** см. [Build tips: dependency installation →](./recipes/build-tips.md#build-troubleshooting)

### CMake опции

| Опция | Описание |
|-------|----------|
| `BUILD_TESTING` | Собрать тесты (default OFF) |
| `GOODNET_STATIC_CORE` | Статическая libgoodnet_core.a (default: SHARED) |
| `GOODNET_STATIC_PLUGINS` | TCP и Logger вкомпилируются в бинарник |
| `GOODNET_COVERAGE` | gcov/lcov инструментация |
| `INSTALL_DEVELOPMENT` | Установить SDK headers и cmake config для сторонних проектов (default OFF) |
| `BUILD_STORE` | Собрать Store — distributed registry сервис (`apps/store`). Требует SQLite3 (default OFF) |

### CMake пресеты

```bash
cmake --preset linux-release && cmake --build --preset linux-release
cmake --preset linux-debug   && cmake --build --preset linux-debug
```

## Почему SHARED

Core собирается как SHARED library по умолчанию. Причина — Logger.

Logger — Meyers singleton (`std::call_once`). Если core статический, каждый `dlopen`-ed плагин получит свою копию Logger с отдельной `once_flag` — два логгера, два файла, потерянные логи.

SHARED гарантирует одну копию Logger в процессе. Core собирается с `ENABLE_EXPORTS ON` (`-rdynamic`) — плагины видят символы ядра (Logger, типы), но не символы друг друга.

## Docker CI (ICE)

```bash
./ci/docker-test.sh ice
```

Поднимает 3-node mesh с локальным coturn STUN-сервером (172.20.0.100:3478). Конфигурация: `ci/config-ice.json`. Docker Compose overlay: `docker-compose.ice.yml`.

**Gotcha**: ICE плагин собирается standalone — у него нет доступа к libsodium. SDK headers не должны включать `sodium.h`.

## Плагины

Плагины ищутся в:
1. `GOODNET_PLUGINS_DIR` (env var) — высший приоритет
2. `plugins.base_dir` — из [конфига](./config.md)
3. `plugins.extra_dirs` — дополнительные каталоги через `;`

Подробности: [Система плагинов](./architecture/plugin-system.md).

---

**См. также:** [Build tips & troubleshooting](./recipes/build-tips.md) · [Быстрый старт](./quickstart.md) · [Конфигурация](./config.md)
