# 14 — Testing

---

## Running Tests

```bash
# Via CMake:
cd build && ctest --output-on-failure

# Directly:
./build/bin/unit_tests
./build/bin/unit_tests --gtest_filter="ConnectionManager.*"

# Via Nix Debug (with coverage):
nix build .#debug
open result/share/coverage/index.html
```

---

## Test Structure

| File | Tests |
|---|---|
| `tests/conf.cpp` | Config: load/save, defaults, type mismatch, fs::path |
| `tests/plugins.cpp` | PluginManager: load, SHA-256, enable/disable, unload |
| `tests/connection_manager.cpp` | Handshake, encrypt/decrypt, replay, routing, localhost |
| `tests/mock_handler.cpp` | `.so` with `handler_init()` for plugin tests |
| `tests/mock_connector.cpp` | `.so` with `connector_init()` for connector tests |

---

## Mock Plugins

```cmake
target_compile_definitions(unit_tests PRIVATE
    MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
    MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
)
```

Tests know the compiled `.so` paths without hardcoding via CMake macros.

---

## Coverage

```bash
nix build .#debug
# lcov collects data, genhtml generates report
# Excludes: /nix/store/*, tests/*, _deps/*
```

lcov flags for GCC 15 compatibility:
```
--ignore-errors inconsistent,unused,mismatch
--rc geninfo_unexecuted_blocks=1
```

---

*← [13 — Build](13-build.md) · [15 — Security →](15-security.md)*
