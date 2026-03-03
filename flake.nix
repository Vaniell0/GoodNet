{
  description = "GoodNet - Modular Network Application Framework";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        lib  = pkgs.lib;

        # ── Утилиты ───────────────────────────────────────────────────────────
        buildPlugin = import ./nix/buildPlugin.nix { inherit lib pkgs; };
        mkCppPlugin = import ./nix/mkCppPlugin.nix { inherit pkgs buildPlugin; };

        # ── Общие зависимости ─────────────────────────────────────────────────
        coreBuildInputs = with pkgs; [ boost spdlog fmt nlohmann_json libsodium ];
        coreNative      = with pkgs; [ cmake ninja pkg-config ];

        # ── Функция сборки core ───────────────────────────────────────────────
        makeCore = { buildType ? "Release", extraFlags ? [] }:
          pkgs.stdenv.mkDerivation {
            pname   = "goodnet-core";
            version = "0.1.0-alpha";
            src     = ./.;

            nativeBuildInputs = coreNative;

            # buildInputs: нужны при сборке ядра.
            # propagatedBuildInputs: spdlog и fmt PUBLIC в cmake-таргете GoodNet::core.
            # GoodNetConfig.cmake вызывает find_dependency(spdlog) и find_dependency(fmt).
            # Плагины не имеют spdlog/fmt в своих buildInputs, но получают их
            # транзитивно через propagatedBuildInputs goodnet-core.
            # Nix cmake hook добавляет propagated deps в CMAKE_PREFIX_PATH потребителя.
            buildInputs            = with pkgs; [ nlohmann_json libsodium boost ];
            propagatedBuildInputs  = with pkgs; [ spdlog fmt ];

            cmakeFlags = [
              "-DINSTALL_DEVELOPMENT=ON"
              "-DCMAKE_BUILD_TYPE=${buildType}"
            ] ++ extraFlags;
          };

        goodnet-core       = makeCore {};
        goodnet-core-debug = makeCore { buildType = "Debug"; };

        # ── Плагины ───────────────────────────────────────────────────────────

        mapPlugins = sdk: type:
          let dir = ./plugins/${type}; in
          if lib.pathExists dir then
            lib.mapAttrs (name: _:
              import (dir + "/${name}/default.nix") {
                inherit pkgs mkCppPlugin;
                goodnetSdk = sdk;
              }
            ) (lib.filterAttrs (n: v:
                v == "directory" && lib.pathExists (dir + "/${n}/default.nix")
              ) (builtins.readDir dir))
          else {};

        makeGroup = name: attrs:
          (pkgs.linkFarm name
            (lib.mapAttrsToList (n: v: { name = n; path = v; }) attrs)
          ) // attrs;

        handlersMap     = mapPlugins goodnet-core "handlers";
        connectorsMap   = mapPlugins goodnet-core "connectors";
        handlersMapDbg  = mapPlugins goodnet-core-debug "handlers";
        connectorsMapDbg= mapPlugins goodnet-core-debug "connectors";

        pluginsTree = makeGroup "plugins-all" {
          handlers   = makeGroup "handlers"   handlersMap;
          connectors = makeGroup "connectors" connectorsMap;
        };

        # ── Bundle плагинов ───────────────────────────────────────────────────

        makeBundle = hmap: cmap:
          pkgs.runCommand "goodnet-plugins-bundle" {} ''
            mkdir -p $out/plugins/handlers $out/plugins/connectors
            collect_to() {
              local src=$1 dest=$2
              [ -d "$src" ] && find -L "$src" -type f \
                \( -name "*.so" -o -name "*.json" \) -exec cp -v {} "$dest/" \;
            }
            ${lib.concatStringsSep "\n" (lib.mapAttrsToList (_: d: ''
              collect_to "${d}" "$out/plugins/handlers"
            '') hmap)}
            ${lib.concatStringsSep "\n" (lib.mapAttrsToList (_: d: ''
              collect_to "${d}" "$out/plugins/connectors"
            '') cmap)}
          '';

        pluginsBundle      = makeBundle handlersMap   connectorsMap;
        pluginsBundleDebug = makeBundle handlersMapDbg connectorsMapDbg;

        # ── Функция финального приложения ────────────────────────────────────

        makeApp = { core, bundle }:
          pkgs.stdenv.mkDerivation {
            pname   = core.pname;
            version = core.version;
            dontUnpack = true;
            dontBuild  = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            installPhase = ''
              mkdir -p $out/bin $out/plugins
              cp ${core}/bin/goodnet     $out/bin/goodnet
              cp -r ${bundle}/plugins/*  $out/plugins/
              wrapProgram $out/bin/goodnet \
                --set LD_LIBRARY_PATH "${lib.makeLibraryPath ([ core ] ++ coreBuildInputs)}" \
                --set GOODNET_PLUGINS_DIR "$out/plugins"
            '';
          };

        # ── Release ───────────────────────────────────────────────────────────
        fullApp = makeApp { core = goodnet-core; bundle = pluginsBundle; };

        # ── Debug ─────────────────────────────────────────────────────────────
        #
        # nix build .#debug
        #
        # Что даёт:
        #   • CMAKE_BUILD_TYPE=Debug → без оптимизаций, с отладочными символами
        #   • NDEBUG не выставлен → LOG_DEBUG/LOG_TRACE/SCOPED_* активны
        #   • console sink включён (вывод в stderr при запуске)
        #   • dontStrip = true → символы не вырезаются → gdb работает полноценно
        #
        # Когда пересобирается, а когда нет:
        #   Nix кеширует деривации по хешу входных данных.
        #   • Исходники не менялись → nix build .#debug возвращает result за <1с
        #   • Изменился один .cpp → пересобирается goodnet-core-debug целиком
        #   • Изменился только плагин → только плагин пересобирается, core кеширован
        #
        # Для по-файловой инкрементальной сборки (изменил один .cpp — пересобрал его):
        #   Используй nix develop + cmake --build build/debug (см. shellHook ниже).
        #   nix build работает на уровне деривации, не файла — это намеренно.
        #
        # После nixos-rebuild switch с impure-derivations в experimental-features
        # можно раскомментировать __impure = true и использовать персистентный
        # build cache → поведение идентично cmake --build но через nix build .#debug.
        #
        fullAppDebug = (makeApp {
          core   = goodnet-core-debug;
          bundle = pluginsBundleDebug;
        }).overrideAttrs (_: {
          pname    = "goodnet-debug";
          dontStrip = true;

          # __impure = true;
          # Раскомментировать ПОСЛЕ: sudo nixos-rebuild switch
          # (нужно: nix.settings.experimental-features = ["impure-derivations"])
          # Тогда nix build .#debug будет использовать persistent build cache
          # и вести себя как make/ninja — пересобирать только изменённые файлы.
        });

      in {
        packages = {
          default = fullApp;
          debug   = fullAppDebug;
          core    = goodnet-core;
          plugins = pluginsTree;
          bundle  = pluginsBundle;
        };

        # ── Dev shell ─────────────────────────────────────────────────────────
        #
        # nix develop  →  среда разработки с cmake, ninja, gdb, ccache
        #
        # Алиасы для быстрой пересборки:
        #   b       → cmake --build build         (release, инкрементальный)
        #   bd      → cmake --build build/debug   (debug, инкрементальный)
        #   cfg     → cmake -B build -DCMAKE_BUILD_TYPE=Release
        #   cfgd    → cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug
        #
        # Это «Makefile»-уровень — только изменённые файлы пересобираются.
        # CCache прозрачно кеширует результаты компиляции между сессиями.

        devShells.default = pkgs.mkShell {
          inputsFrom = [ goodnet-core ];
          packages   = with pkgs; [ gdb ccache cmake-format jq ];

          shellHook = ''
            export GOODNET_SDK_PATH="${goodnet-core}/sdk"

            # ccache: кеш компиляции между сессиями nix develop
            # ~/.cache/ccache хранит скомпилированные объекты
            export CCACHE_DIR="$HOME/.cache/ccache"
            export CMAKE_C_COMPILER_LAUNCHER=ccache
            export CMAKE_CXX_COMPILER_LAUNCHER=ccache

            # ── Алиасы быстрой сборки ─────────────────────────────────────────
            # Release
            cfg()  { cmake -B build       -DCMAKE_BUILD_TYPE=Release -G Ninja "$@"; }
            b()    { cmake --build build  "$@"; }
            brun() { cmake --build build && ./build/goodnet; }

            # Debug (инкрементальный — только изменённые файлы)
            cfgd() { cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug   -G Ninja "$@"; }
            bd()   { cmake --build build/debug "$@"; }
            bdrun(){ cmake --build build/debug && ./build/debug/goodnet; }

            # Первый запуск: cfgd && bd
            # Далее: bd (только изменённые файлы)
            echo "GoodNet dev shell. Commands: cfg/b/brun (release), cfgd/bd/bdrun (debug)"
          '';
        };
      });
}
