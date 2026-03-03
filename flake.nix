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
          let
            isDebug = buildType == "Debug";
          in
          pkgs.stdenv.mkDerivation {
            pname   = "goodnet-core";
            version = "0.1.0-alpha";
            src     = ./.;

            nativeBuildInputs = coreNative ++ [ pkgs.gtest ] ++ (lib.optional isDebug pkgs.lcov);
            buildInputs = with pkgs; [ nlohmann_json libsodium boost gtest ];
            propagatedBuildInputs  = with pkgs; [ spdlog fmt ];

            # Убираем дефолтный билд-тайп Nix, чтобы наш точно прошел первым
            forceNoRelease = isDebug; 

            cmakeFlags = [
              "-DINSTALL_DEVELOPMENT=ON"
              "-DCMAKE_BUILD_TYPE=${buildType}"
              "-DBUILD_TESTING=ON"
              "-DGOODNET_DISABLE_PCH=ON"
            ] ++ (lib.optionals isDebug [
              "-DCMAKE_CXX_FLAGS=--coverage"
              "-DCMAKE_EXE_LINKER_FLAGS=--coverage"
            ]) ++ extraFlags;

            doCheck = true;
            checkPhase = ''
              export HOME=$TMPDIR
              export TMPDIR=$TMPDIR   # уже установлен Nix sandbox

              ${lib.optionalString isDebug ''
                lcov --directory . --zerocounters
              ''}

              ./bin/unit_tests --gtest_output="xml:test_results.xml"

              ${lib.optionalString isDebug ''
                # Собираем данные, игнорируя несоответствия GCC 15
                lcov --directory . \
                     --capture \
                     --output-file coverage.info \
                     --ignore-errors inconsistent,unused,mismatch \
                     --rc geninfo_unexecuted_blocks=1

                # Чистим от системного мусора
                lcov --remove coverage.info '/nix/store/*' '*/tests/*' '*/_deps/*' \
                     --output-file coverage_cleaned.info \
                     --ignore-errors inconsistent,unused,mismatch

                # Просто выводим в консоль для информации
                lcov --list coverage_cleaned.info --ignore-errors inconsistent,unused,mismatch
              ''}
            '';

            postInstall = ''
              mkdir -p $out/share/test-results
              cp test_results.xml $out/share/test-results/
              
              ${lib.optionalString isDebug ''
                mkdir -p $out/share/coverage
                genhtml coverage_cleaned.info \
                        --output-directory $out/share/coverage \
                        --ignore-errors inconsistent,unused,mismatch
              ''}
            '';
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
              if [ -d "${core}/share" ]; then
                cp -r ${core}/share $out/share
              fi
              wrapProgram $out/bin/goodnet \
                --set LD_LIBRARY_PATH "${lib.makeLibraryPath ([ core ] ++ coreBuildInputs)}" \
                --set GOODNET_PLUGINS_DIR "$out/plugins"
            '';
          };

        # ── Release ───────────────────────────────────────────────────────────
        fullApp = makeApp { core = goodnet-core; core-debug = goodnet-core-debug; bundle = pluginsBundle; };

        # ── Debug ─────────────────────────────────────────────────────────────
        #
        # nix build .#debug
        #
        fullAppDebug = (makeApp {
          core   = goodnet-core-debug;
          bundle = pluginsBundleDebug;
        }).overrideAttrs (old: {
          pname    = "goodnet-debug";
          dontStrip = true;

          # Обертка, чтобы gcov не пытался писать по путям сборки
          nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.makeWrapper ];
          
          postFixup = ''
            wrapProgram $out/bin/goodnet \
              --set GCOV_ERROR_FILE /dev/null \
              --set GCOV_PREFIX /tmp/goodnet-coverage \
              --set GCOV_PREFIX_STRIP 10
          '';
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
