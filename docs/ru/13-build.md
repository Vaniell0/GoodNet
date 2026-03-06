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
nix build .#plugins    # все плагины как отдельные derivations
nix build .#bundle     # только bundle плагинов
```

---

## CMake targets

| Target | Тип | Описание |
|---|---|---|
| `goodnet_sdk` | INTERFACE | Заголовки SDK для плагинов |
| `goodnet_core` | SHARED | libgoodnet_core.so |
| `goodnet` | EXECUTABLE | Основной бинарник |
| `unit_tests` | EXECUTABLE | GTest suite |
| `mock_handler` | SHARED | Mock handler для тестов |
| `mock_connector` | SHARED | Mock connector для тестов |

### Почему SHARED для core?

```
goodnet_core SHARED  →  одна копия статических переменных
                         Logger singleton (std::call_once флаг)
                         Всегда shared между ядром и плагинами
```

STATIC core: каждый плагин получал бы свою копию Logger → несколько независимых синглтонов → логи идут в разные файлы.

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

`buildPlugin.nix` автоматически вычислит SHA-256 и создаст `my_handler.so.json`.

Плагин появится в:
- `packages.plugins.handlers.my_handler`
- Включён в `packages.bundle` (скопирован в `$out/plugins/handlers/`)

---

## ccache

В dev shell ccache настроен автоматически:

```bash
export CCACHE_DIR="$HOME/.cache/ccache"
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Повторная сборка после `nix develop` в новой сессии использует кэш → ускорение в 10–100x.

---

*← [12 — C++ SDK](12-sdk-cpp.md) · [14 — Тестирование →](14-testing.md)*
