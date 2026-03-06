# 13 — Build

---

## Nix Flakes (recommended)

```bash
nix develop      # dev env: cmake, ninja, gdb, ccache, jq

# Aliases inside dev shell:
cfg   → cmake -B build       -DCMAKE_BUILD_TYPE=Release -G Ninja
b     → cmake --build build
brun  → cmake --build build && ./build/goodnet

cfgd  → cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -G Ninja
bd    → cmake --build build/debug
bdrun → cmake --build build/debug && ./build/debug/goodnet
```

### Nix packages

```bash
nix build              # default: core + all plugins + bundle
nix build .#debug      # Debug with coverage report
nix build .#core       # libgoodnet_core only
nix build .#plugins    # all plugins as separate derivations
nix build .#bundle     # plugin bundle only
```

---

## CMake Targets

| Target | Type | Description |
|---|---|---|
| `goodnet_sdk` | INTERFACE | SDK headers for plugins |
| `goodnet_core` | SHARED | libgoodnet_core.so |
| `goodnet` | EXECUTABLE | Main binary |
| `unit_tests` | EXECUTABLE | GTest suite |
| `mock_handler` | SHARED | Mock handler for tests |
| `mock_connector` | SHARED | Mock connector for tests |

### Why SHARED for core?

STATIC core: each plugin gets its own copy of Logger singleton → multiple independent singletons → logs go to different files. SHARED core: one copy of all static variables, shared between core and all plugins.

---

## Adding a Plugin

### Via CMake

```cmake
# plugins/handlers/my_handler/CMakeLists.txt
add_library(my_handler SHARED my_handler.cpp)
target_link_libraries(my_handler PRIVATE goodnet_core)
```

### Via Nix (for distribution)

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

`buildPlugin.nix` automatically computes SHA-256 and creates `my_handler.so.json`.

---

*← [12 — C++ SDK](12-sdk-cpp.md) · [14 — Testing →](14-testing.md)*
