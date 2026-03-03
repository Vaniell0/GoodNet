# GoodNet — Build System

GoodNet uses **Nix Flakes** as the top-level build system and **CMake + Ninja** as the C++ build tool. Nix provides reproducibility and hermetic dependency management; CMake handles compilation details that Nix delegates to it.

---

## Quick Start

```bash
# Enter the development shell (sets up PATH, cmake, ninja, ccache, SDK path)
nix develop

# Configure and build incrementally (fast, file-level)
cfgd && bd          # Debug
cfg  && b           # Release

# Run
./build/debug/goodnet
./build/goodnet

# Full reproducible build (Nix store output)
nix build           # Release → ./result/bin/goodnet
nix build .#debug   # Debug   → ./result/bin/goodnet
```

---

## Nix Flake Structure

```
flake.nix
nix/
├── mkCppPlugin.nix    # Builds one plugin from source
└── buildPlugin.nix    # Signs plugin, generates .so.json manifest
```

### Derivation graph

```
nixpkgs (boost, spdlog, fmt, nlohmann_json, libsodium)
    │
    ▼
goodnet-core          ← libgoodnet_core.so + headers + cmake SDK
    │
    ├─► handlers/logger    ← liblogger.so + liblogger.so.json
    ├─► handlers/...
    ├─► connectors/tcp     ← libtcp.so + libtcp.so.json
    └─► connectors/...
         │
         ▼
    pluginsBundle      ← flat directory: handlers/*.so, connectors/*.so
         │
         ▼
    fullApp            ← bin/goodnet + plugins/ + wrapProgram env vars
```

Every derivation is content-addressed. If source or dependencies have not changed, Nix reuses the cached output from `/nix/store` instantly. If one plugin changes, only that plugin is rebuilt — `goodnet-core` and all other plugins are pulled from cache.

---

## Named Packages

```bash
nix build               # .#default — full Release app
nix build .#debug       # Debug app with symbols, console log output
nix build .#core        # libgoodnet_core.so + SDK only
nix build .#plugins     # all plugins (browsable tree)
nix build .#bundle      # flat plugins directory
```

Access individual plugins:
```bash
nix build .#plugins.handlers.logger
nix build .#plugins.connectors.tcp
```

---

## `goodnet-core` Derivation

```nix
goodnet-core = pkgs.stdenv.mkDerivation {
  pname = "goodnet-core";
  version = "0.1.0-alpha";
  src = ./.;

  nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];

  buildInputs           = with pkgs; [ nlohmann_json libsodium boost ];
  propagatedBuildInputs = with pkgs; [ spdlog fmt ];
  # propagated: plugins find spdlog/fmt transitively via CMAKE_PREFIX_PATH

  cmakeFlags = [
    "-DINSTALL_DEVELOPMENT=ON"    # installs SDK headers + GoodNetConfig.cmake
    "-DCMAKE_BUILD_TYPE=Release"
  ];
};
```

`propagatedBuildInputs` is the key difference from plain `buildInputs`. When a downstream derivation (a plugin) has `goodnet-core` in its `buildInputs`, Nix automatically adds `spdlog` and `fmt` to that derivation's `CMAKE_PREFIX_PATH`. This is necessary because `GoodNetConfig.cmake` calls `find_dependency(spdlog)` and `find_dependency(fmt)` — those packages must be findable when the plugin's CMake runs.

`nlohmann_json` and `libsodium` are **not** propagated because they are PRIVATE dependencies of the core (used only in `.cpp` files). Plugins never need to find them.

---

## Plugin Build Pipeline: `mkCppPlugin`

`nix/mkCppPlugin.nix` is the factory function called from each plugin's `default.nix`:

```nix
# plugins/handlers/logger/default.nix
{ pkgs, mkCppPlugin, goodnetSdk, ... }:

mkCppPlugin {
  name        = "logger";
  type        = "handlers";      # determines install subdirectory
  version     = "1.0.0";
  description = "Records incoming packets to binary bundle files";
  src         = ./.;
  deps        = [];              # extra Nix packages, e.g. pkgs.boost
  inherit goodnetSdk;
}
```

Internally, `mkCppPlugin` does two things:

**Step 1 — `rawBuild`:** Compiles the plugin with CMake:
```
cmake -DCMAKE_PREFIX_PATH=${goodnetSdk} -DBUILD_SHARED_LIBS=ON ...
```
This makes `find_package(GoodNet REQUIRED)` work by pointing CMake at the installed SDK.

**Step 2 — `buildPlugin`:** Takes the compiled `.so`, computes its SHA-256, and writes a JSON manifest:
```json
{
  "meta": { "name": "logger", "type": "handlers", "version": "1.0.0", ... },
  "integrity": { "alg": "sha256", "hash": "abc123..." }
}
```
The manifest file is named `liblogger.so.json` (appended, not replacing the extension). `PluginManager` reads and verifies this before calling `dlopen`.

---

## Debug Build: `nix build .#debug`

The debug package uses the same `makeCore` function with `buildType = "Debug"`:

```nix
goodnet-core-debug = makeCore { buildType = "Debug"; };
```

Effects of `CMAKE_BUILD_TYPE=Debug`:
- No `-O2`/`-O3` — symbols are not elided, GDB can step through everything
- `NDEBUG` is not defined → `LOG_DEBUG`, `LOG_TRACE`, `SCOPED_TRACE()` are active
- Console sink is compiled in (see `#ifndef NDEBUG` in `logger.cpp`)
- PCH is enabled (see below) — incremental debug builds are fast

Debug plugins are built against `goodnet-core-debug` so `NDEBUG` state is consistent.

**Caching behaviour:** `nix build .#debug` is fully cached when sources have not changed. Running it a second time without modifying any file returns in under a second.

**After `nixos-rebuild switch` with `impure-derivations` enabled**, uncomment `__impure = true` in `flake.nix` to get truly incremental file-level compilation through a persistent CMake cache directory in `$HOME`.

---

## Development Shell

```bash
nix develop
```

This drops into a shell with the full build environment. The `shellHook` sets up:

| Variable | Value |
|---|---|
| `GOODNET_SDK_PATH` | Path to installed SDK in Nix store |
| `CCACHE_DIR` | `~/.cache/ccache` — ccache reuses object files across sessions |
| `CMAKE_CXX_COMPILER_LAUNCHER` | `ccache` — transparent, no CMake changes needed |

Shell aliases for fast iteration:

| Alias | Command | Notes |
|---|---|---|
| `cfg` | `cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja` | Configure Release |
| `b` | `cmake --build build` | Build Release (incremental) |
| `brun` | `cmake --build build && ./build/goodnet` | Build + run |
| `cfgd` | `cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -G Ninja` | Configure Debug |
| `bd` | `cmake --build build/debug` | Build Debug (incremental, file-level) |
| `bdrun` | `cmake --build build/debug && ./build/debug/goodnet` | Build + run |

First-time setup:
```bash
cfgd    # configure once
bd      # build (slow first time, fast after)
bdrun   # build + run
```

Subsequent edits to a single `.cpp`:
```bash
bd      # only changed files recompile
```

---

## CMake Structure

```
CMakeLists.txt
cmake/
├── pch.cmake              # Precompiled Header setup (Debug only)
├── GoodNetConfig.cmake.in # Template for installed SDK config
└── gen_manifests.cmake    # Target: generate .so.json for manual builds
```

### Key CMake targets

| Target | Type | Description |
|---|---|---|
| `goodnet_sdk` | INTERFACE library | SDK headers only, no compiled code |
| `goodnet_core` | SHARED library | Core framework, SOVERSION 0 |
| `goodnet` | Executable | Main application |

### `add_plugin()` (from `helper.cmake`)

Used in every plugin's `CMakeLists.txt`. Sets:
- `PREFIX "lib"` → `libmyplugin.so`
- `CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN ON` → `-fvisibility=hidden`
- `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections` → minimal `.so` size
- Calls `apply_plugin_pch()` if `CMAKE_BUILD_TYPE == Debug`

---

## Precompiled Headers (PCH)

`cmake/pch.cmake` is included from the root `CMakeLists.txt` and provides two functions:

- `apply_pch(target)` — used for `goodnet_core` and `goodnet` executable
- `apply_plugin_pch(target)` — used inside `add_plugin()` for plugins

Both are **no-ops in Release**. In Debug they call `target_precompile_headers()` with the heavy headers:

```
spdlog/spdlog.h          ← saved ~0.8s per TU
nlohmann/json.hpp        ← saved ~1.2s per TU
fmt/format.h             ← saved ~0.3s per TU
STL (string, vector, memory, mutex, filesystem, ...)
```

The PCH is compiled once per build directory. On the second `bd` run, all these headers are served from the `.gch` file — only `.cpp` files that actually changed are recompiled.

---

## Dependency Summary

| Package | Used by | Propagated |
|---|---|---|
| `boost` | core (signals, asio), tcp plugin | yes |
| `spdlog` | core logger | yes |
| `fmt` | core, plugins | yes |
| `nlohmann_json` | core config parser | no (PRIVATE) |
| `libsodium` | plugin manifest SHA-256 | no (PRIVATE) |
| `cmake`, `ninja`, `pkg-config` | build only (nativeBuildInputs) | no |
| `makeWrapper` | fullApp wrapping only | no |
