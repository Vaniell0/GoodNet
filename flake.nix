{
  description = "GoodNet - Modular Network Application Framework";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        lib  = pkgs.lib;

        buildPlugin = import ./nix/buildPlugin.nix { inherit lib pkgs; };
        mkCppPlugin = import ./nix/mkCppPlugin.nix { inherit pkgs buildPlugin; };

        # Filter out build artifacts and editor state from source copies.
        cleanSrc = lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let b = builtins.baseNameOf path; in
            !(b == "build" || b == "result" || b == ".git" || b == ".direnv");
        };

        coreBuildInputs = with pkgs; [ boost spdlog fmt nlohmann_json libsodium zstd ];
        coreNative      = with pkgs; [ cmake ninja pkg-config ];

        # ── Core ──────────────────────────────────────────────────────────────
        makeCore = { buildType ? "Release", extraFlags ? [] }:
          pkgs.stdenv.mkDerivation {
            pname   = "goodnet-core";
            version = "0.1.0-alpha";
            src     = cleanSrc;

            nativeBuildInputs = coreNative ++ [ pkgs.gtest ];
            buildInputs = with pkgs; [
              fmt nlohmann_json libsodium zstd boost gtest
            ];
            propagatedBuildInputs = with pkgs; [ spdlog ];

            cmakeFlags = [
              "-DINSTALL_DEVELOPMENT=ON"
              "-DCMAKE_BUILD_TYPE=${buildType}"
              "-DBUILD_TESTING=ON"
              "-DGOODNET_DISABLE_PCH=ON"
            ] ++ extraFlags;

            doCheck = true;
            checkPhase = ''
              export HOME=$TMPDIR
              ./bin/unit_tests --gtest_output="xml:test_results.xml"
            '';

            postInstall = ''
              mkdir -p $out/share/test-results
              cp test_results.xml $out/share/test-results/
            '';
          };

        goodnet-core = makeCore {};

        # ── Plugins ──────────────────────────────────────────────────────────
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

        handlersMap   = mapPlugins goodnet-core "handlers";
        connectorsMap = mapPlugins goodnet-core "connectors";

        pluginsTree = makeGroup "plugins-all" {
          handlers   = makeGroup "handlers"   handlersMap;
          connectors = makeGroup "connectors" connectorsMap;
        };

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

        pluginsBundle = makeBundle handlersMap connectorsMap;

        # ── Full App ──────────────────────────────────────────────────────────
        makeApp = { core, bundle }:
          pkgs.stdenv.mkDerivation {
            pname   = core.pname;
            version = core.version;
            dontUnpack = true;
            dontBuild  = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            installPhase = ''
              mkdir -p $out/bin $out/plugins
              cp ${core}/bin/goodnet    $out/bin/goodnet
              cp -r ${bundle}/plugins/* $out/plugins/
              if [ -d "${core}/share" ]; then cp -r ${core}/share $out/share; fi
              wrapProgram $out/bin/goodnet \
                --set LD_LIBRARY_PATH \
                    "${lib.makeLibraryPath ([ core ] ++ coreBuildInputs)}" \
                --set GOODNET_PLUGINS_DIR "$out/plugins"
            '';
          };

        fullApp = makeApp { core = goodnet-core; bundle = pluginsBundle; };

        # ── Docker ────────────────────────────────────────────────────────────
        dockerImage = pkgs.dockerTools.buildLayeredImage {
          name     = "goodnet-docker";
          tag      = "latest";
          contents = [ fullApp pkgs.cacert pkgs.bashInteractive pkgs.coreutils ];
          config   = {
            Entrypoint = [ "${fullApp}/bin/goodnet" ];
            WorkingDir = "/data";
            Env = [
              "GOODNET_PLUGINS_DIR=${fullApp}/plugins"
              "LD_LIBRARY_PATH=${lib.makeLibraryPath ([ fullApp ] ++ coreBuildInputs)}"
            ];
            Volumes = { "/data" = {}; };
          };
        };

        # ── Local development scripts (nix run) ──────────────────────────────
        # These scripts build in the project directory (not /nix/store) with
        # PCH enabled (Debug mode, GOODNET_DISABLE_PCH=OFF by default).

        devBuildInputs = coreNative ++ coreBuildInputs ++ [ pkgs.gtest ];

        gn-dev = pkgs.writeShellApplication {
          name = "gn-dev";
          runtimeInputs = devBuildInputs;
          text = ''
            BUILD_DIR="build"
            if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
              echo ">>> Configuring Debug build (PCH enabled)..."
              cmake -B "$BUILD_DIR" \
                -DCMAKE_BUILD_TYPE=Debug \
                -G Ninja \
                -DBUILD_TESTING=ON
            fi
            cmake --build "$BUILD_DIR" -j"$(nproc)"
            exec "./$BUILD_DIR/goodnet" "$@"
          '';
        };

        gn-test = pkgs.writeShellApplication {
          name = "gn-test";
          runtimeInputs = devBuildInputs;
          text = ''
            BUILD_DIR="build"
            if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
              echo ">>> Configuring Debug build (PCH enabled)..."
              cmake -B "$BUILD_DIR" \
                -DCMAKE_BUILD_TYPE=Debug \
                -G Ninja \
                -DBUILD_TESTING=ON
            fi
            cmake --build "$BUILD_DIR" -j"$(nproc)"
            exec "./$BUILD_DIR/bin/unit_tests" "$@"
          '';
        };

        gn-coverage = pkgs.writeShellApplication {
          name = "gn-coverage";
          runtimeInputs = devBuildInputs ++ [ pkgs.lcov ];
          text = ''
            BUILD_DIR="build/coverage"

            echo ">>> Configuring coverage build..."
            cmake -B "$BUILD_DIR" \
              -DCMAKE_BUILD_TYPE=Debug \
              -G Ninja \
              -DBUILD_TESTING=ON \
              -DGOODNET_COVERAGE=ON

            cmake --build "$BUILD_DIR" -j"$(nproc)"

            echo ">>> Resetting coverage counters..."
            lcov --zerocounters --directory "$BUILD_DIR"

            echo ">>> Running tests..."
            HOME="$(mktemp -d)" "./$BUILD_DIR/bin/unit_tests" \
              --gtest_output="xml:$BUILD_DIR/test_results.xml" || true

            echo ">>> Capturing coverage data..."
            lcov --capture --directory "$BUILD_DIR" \
              --output-file "$BUILD_DIR/coverage.info" \
              --ignore-errors mismatch,inconsistent

            echo ">>> Filtering external sources..."
            lcov --remove "$BUILD_DIR/coverage.info" \
              '/nix/*' '*/tests/*' '*/gtest/*' '*/boost/*' '*/nlohmann/*' \
              --output-file "$BUILD_DIR/coverage_filtered.info" \
              --ignore-errors unused,mismatch,inconsistent

            echo ">>> Generating HTML report..."
            genhtml "$BUILD_DIR/coverage_filtered.info" \
              --output-directory "$BUILD_DIR/coverage_html" \
              --title "GoodNet Coverage"

            echo ""
            echo "=== Coverage Report ==="
            lcov --summary "$BUILD_DIR/coverage_filtered.info" \
              --ignore-errors empty 2>&1 || true
            echo ""
            echo "HTML report: $BUILD_DIR/coverage_html/index.html"
          '';
        };

      in {
        packages = {
          default = fullApp;
          core    = goodnet-core;
          plugins = pluginsTree;
          docker  = dockerImage;
        };

        apps = {
          default = {
            type    = "app";
            program = "${gn-dev}/bin/gn-dev";
          };
          test = {
            type    = "app";
            program = "${gn-test}/bin/gn-test";
          };
          coverage = {
            type    = "app";
            program = "${gn-coverage}/bin/gn-coverage";
          };
          docker-load = {
            type    = "app";
            program = "${pkgs.writeShellScriptBin "docker-load" ''
              ${pkgs.docker}/bin/docker load < ${dockerImage}
            ''}/bin/docker-load";
          };
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ goodnet-core ];
          packages = with pkgs; [
            gdb ccache cmake-format jq
            include-what-you-use   # linting tool, not needed for build
            lcov                   # coverage report generation
          ];

          shellHook = ''
            export GOODNET_SDK_PATH="${goodnet-core}/sdk"
            export CCACHE_DIR="$HOME/.cache/ccache"
            export CMAKE_C_COMPILER_LAUNCHER=ccache
            export CMAKE_CXX_COMPILER_LAUNCHER=ccache

            cfg()   { cmake -B build       -DCMAKE_BUILD_TYPE=Release -G Ninja "$@"; }
            b()     { cmake --build build  "$@"; }
            brun()  { cmake --build build  && ./build/goodnet "$@"; }
            cfgd()  { cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug   -G Ninja "$@"; }
            bd()    { cmake --build build/debug "$@"; }
            bdrun() { cmake --build build/debug && ./build/debug/goodnet "$@"; }

            echo ""
            echo "GoodNet devShell (Linux/Native)"
            echo "  cfg / b / brun    - Release build & run"
            echo "  cfgd / bd / bdrun - Debug build & run"
            echo "  nix run           - Debug build & run (PCH, replaces aliases)"
            echo "  nix run .#test    - Build & run unit tests"
            echo "  nix run .#coverage - Coverage report"
            echo ""
          '';
        };
      });
}
