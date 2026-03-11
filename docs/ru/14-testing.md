# 14 — Тестирование

---

## Запуск

```bash
# Через CMake:
cd build && ctest --output-on-failure

# Напрямую:
./build/bin/unit_tests
./build/bin/unit_tests --gtest_filter="Core.*"
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
| `tests/core.cpp` | `gn::Core`: lifecycle (run/stop), `subscribe()`, `send()`, C API (`gn_core_create/destroy/run_async/stop`) |
| `tests/conf.cpp` | Config: load/save, defaults, type mismatch, fs::path |
| `tests/plugins.cpp` | PluginManager: load, SHA-256 verify, priority, enable/disable, unload |
| `tests/connection_manager.cpp` | Handshake, encrypt/decrypt, replay protection, routing, localhost, StatsCollector, `rotate_identity_keys` |
| `tests/mock_handler.cpp` | `.so` с `handler_init()` + `on_message_result()` для тестов |
| `tests/mock_connector.cpp` | `.so` с `connector_init()` для тестов |

---

## Ключевые тест-кейсы Core

```cpp
// tests/core.cpp

// Lifecycle
TEST(Core, StartStop) {
    gn::Core core;
    core.run_async(1);
    EXPECT_TRUE(core.is_running());
    core.stop();
    EXPECT_FALSE(core.is_running());
}

// Subscription + dispatch
TEST(Core, SubscribeReceive) {
    gn::Core core;
    core.run_async(1);

    std::atomic<int> count{0};
    core.subscribe(MSG_TYPE_CHAT, "test",
        [&](auto, auto, auto, auto) {
            ++count;
            return PROPAGATION_CONSUMED;
        });

    core.send("tcp://127.0.0.1:25566", MSG_TYPE_CHAT, "hello");
    // ... ожидание и проверка count
    core.stop();
}

// C API
TEST(Core, CApi) {
    gn_config_t cfg{ .log_level = "warn" };
    gn_core_t* c = gn_core_create(&cfg);
    ASSERT_NE(c, nullptr);

    gn_core_run_async(c, 1);
    char buf[65];
    size_t len = gn_core_get_user_pubkey(c, buf, sizeof(buf));
    EXPECT_EQ(len, 64u);

    gn_core_stop(c);
    gn_core_destroy(c);
}
```

---

## Mock плагины

```cmake
# CMakeLists.txt — пути передаются через define:
target_compile_definitions(unit_tests PRIVATE
    MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
    MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
)
```

Тесты знают пути к `.so` без хардкода. `mock_handler` реализует `on_message_result()` → позволяет тестировать `PROPAGATION_CONSUMED`, `PROPAGATION_REJECT`, и закрепление `affinity_plugin`.

---

## Coverage (Debug)

```bash
nix build .#debug
# lcov собирает данные, genhtml генерирует HTML отчёт
# Исключаются: /nix/store/*, tests/*, _deps/*
```

lcov флаги для GCC 15:
```
--ignore-errors inconsistent,unused,mismatch
--rc geninfo_unexecuted_blocks=1
```

---

*← [13 — Сборка](13-build.md) · [15 — Безопасность →](15-security.md)*