# 14 — Тестирование

---

## Запуск

```bash
# Через CMake:
cd build && ctest --output-on-failure

# Напрямую:
./build/bin/unit_tests
./build/bin/unit_tests --gtest_filter="ConnectionManager.*"
./build/bin/unit_tests --gtest_output="xml:results.xml"

# Через Nix Debug (с coverage):
nix build .#debug
ls result/share/coverage/index.html  # HTML отчёт lcov
```

---

## Структура тестов

| Файл | Что тестирует |
|---|---|
| `tests/conf.cpp` | Config: load/save, defaults, type mismatch, fs::path |
| `tests/plugins.cpp` | PluginManager: load, SHA-256 verify, enable/disable, unload |
| `tests/connection_manager.cpp` | Handshake, encrypt/decrypt, replay, routing, localhost |
| `tests/mock_handler.cpp` | `.so` с `handler_init()` для тестов PluginManager |
| `tests/mock_connector.cpp` | `.so` с `connector_init()` для тестов |

---

## Mock плагины

```cmake
# CMakeLists.txt
target_compile_definitions(unit_tests PRIVATE
    MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
    MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
)
```

Тесты знают пути к скомпилированным `.so` без хардкода через макросы.

---

## Coverage (Debug)

```bash
nix build .#debug
# lcov собирает данные, genhtml генерирует отчёт
# Исключаются: /nix/store/*, tests/*, _deps/*
```

lcov флаги для совместимости с GCC 15:
```
--ignore-errors inconsistent,unused,mismatch
--rc geninfo_unexecuted_blocks=1
```

---

*← [13 — Сборка](13-build.md) · [15 — Безопасность →](15-security.md)*
