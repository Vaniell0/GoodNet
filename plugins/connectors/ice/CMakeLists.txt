cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 23)
project(goodnet-ice)

find_package(GoodNet REQUIRED)
cmake_policy(SET CMP0167 NEW)

# ── libnice + GLib ────────────────────────────────────────────────────────────
# libnice ships pkg-config files; CMake's FindPkgConfig is the canonical way.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NICE  REQUIRED IMPORTED_TARGET nice)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO   REQUIRED IMPORTED_TARGET gio-2.0)

# ── Plugin target ─────────────────────────────────────────────────────────────
add_plugin(ice ice.cpp)

target_link_libraries(ice
  PRIVATE
    GoodNet::core
    PkgConfig::NICE
    PkgConfig::GLIB2
    PkgConfig::GIO
)

# GLib headers define G_DISABLE_DEPRECATED in some builds; suppress noisy
# warnings that come from glib internals when building with -Wall.
target_compile_options(ice PRIVATE
  -Wno-deprecated-declarations
)