{
  description = "Advanced C++ Development Environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        libs = with pkgs; [ 
          fmt spdlog boost nlohmann_json libsodium
        ];

        sdk = pkgs.stdenv.mkDerivation {
          name = "goodnet-sdk";
          src = ./sdk;
          nativeBuildInputs = [ pkgs.cmake ];
          installPhase = ''
            mkdir -p $out/include/goodnet
            cp -r *.h cpp/*.hpp $out/include/goodnet/ 2>/dev/null || true
          '';
        };

        buildGoodNet = { buildType ? "Release", useUpx ? false, extraFlags ? [] }:
          let
            upxPhase = if useUpx && buildType != "Debug" then ''
              echo "📦 Compressing binary with UPX..."
              ${pkgs.upx}/bin/upx --best --lzma $out/bin/goodnet
            '' else "";
          in
          pkgs.stdenv.mkDerivation {
            pname = "goodnet" + (if useUpx then "-upx" else "") + "-" + (buildType);
            version = "0.1.0";
            src = self;

            nativeBuildInputs = with pkgs; [ 
              cmake ninja pkg-config 
            ] ++ (if useUpx then [ upx ] else []);

            buildInputs = libs;

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=${buildType}"
              "-DBUILD_PLUGINS=OFF"
              "-DUSE_UPX=${if useUpx then "ON" else "OFF"}"
            ] ++ extraFlags;

            installPhase = ''
              mkdir -p $out/bin
              find . -name goodnet -type f -executable -exec cp {} $out/bin/ \;
              ${upxPhase}
            '';

            postInstall = ''
              echo "📏 Binary size:"
              ls -lh $out/bin/goodnet
            '';
          };

        goodnet-release = buildGoodNet {
          buildType = "Release";
          useUpx = false;
        };

        goodnet-release-upx = buildGoodNet {
          buildType = "Release";
          useUpx = true;
        };

        goodnet-minsizerel = buildGoodNet {
          buildType = "MinSizeRel";
          useUpx = false;
        };

        goodnet-minsizerel-upx = buildGoodNet {
          buildType = "MinSizeRel";
          useUpx = true;
        };

      in
      {
        packages = {
          default = goodnet-release;
          sdk = sdk;
          
          release = goodnet-release;
          release-upx = goodnet-release-upx;
          minsize = goodnet-minsizerel;
          minsize-upx = goodnet-minsizerel-upx;
          
          plugin-handlers = pkgs.stdenv.mkDerivation {
            name = "goodnet-handlers";
            src = ./plugins/handlers;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
            buildInputs = libs ++ [ sdk ];
            cmakeFlags = [ "-DPLUGIN_BUILD=ON" ];
            installPhase = "mkdir -p $out";
          };
          
          plugin-connectors = pkgs.stdenv.mkDerivation {
            name = "goodnet-connectors";
            src = ./plugins/connectors;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
            buildInputs = libs ++ [ sdk ];
            cmakeFlags = [ "-DPLUGIN_BUILD=ON" ];
            installPhase = "mkdir -p $out";
          };
        };

        devShells.default = pkgs.mkShell {
          name = "goodnet-dev";
          
          nativeBuildInputs = with pkgs; [ 
            cmake pkg-config gdb ninja gcc14 ccache upx
          ];
          
          buildInputs = libs;
          
          shellHook = ''
            echo "🚀 GoodNet Development Environment"
            echo "Build types: Debug (default), Release, MinSizeRel"
            
            export CCACHE_DIR="$HOME/.ccache/goodnet"
            mkdir -p "$CCACHE_DIR"
            
            export NPROC=$(($(nproc) > 1 ? $(nproc) - 2 : 1))
            
            # 🔧 Функция сборки
            build_project() {
              local build_type=''${1:-Debug}
              local use_upx=''${2:-OFF}
              local build_plugins=''${3:-OFF}
              
              echo "🔨 Building $build_type (UPX: $use_upx, Plugins: $build_plugins)..."
              
              mkdir -p build
              cd build
              cmake .. -G Ninja \
                -DCMAKE_BUILD_TYPE=$build_type \
                -DUSE_UPX=$use_upx \
                -DBUILD_PLUGINS=$build_plugins \
                -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
                -DCMAKE_C_COMPILER_LAUNCHER=ccache
              ninja -j$NPROC
              cd ..
            }
            
            # 🎯 Специализированные команды
            build-debug() {
              build_project "Debug" "OFF" "OFF"
            }
            
            build-min() {
              build_project "MinSizeRel" "ON" "OFF"
            }
            
            build-release() {
              build_project "Release" "ON" "OFF"
            }
            
            build-plugins() {
              echo "🔌 Building plugins..."
              cd plugins/handlers && build_project "Debug" "OFF" "ON" && cd ../..
              cd plugins/connectors && build_project "Debug" "OFF" "ON" && cd ../..
            }
            
            build-all() {
              build-debug
              build-plugins
            }
            
            # 📊 Сравнение размеров
            compare-sizes() {
              echo "📊 Comparing binary sizes:"
              echo "=========================="
              
              # Собираем разные версии
              build-release 2>/dev/null && \
                echo "Release:     $(stat -c%s build/bin/goodnet) bytes"
              
              build-min 2>/dev/null && \
                echo "MinSizeRel:  $(stat -c%s build/bin/goodnet) bytes"
              
              build-min-upx 2>/dev/null && \
                echo "MinSizeRel+UPX: $(stat -c%s build/bin/goodnet) bytes"
              
              # Тест UPX
              if [ -f build/bin/goodnet ]; then
                echo ""
                echo "UPX compression test on Release build:"
                cp build/bin/goodnet build/bin/goodnet.release
                upx --best --lzma build/bin/goodnet.release -o build/bin/goodnet.upx 2>/dev/null
                original=$(stat -c%s build/bin/goodnet.release)
                compressed=$(stat -c%s build/bin/goodnet.upx)
                reduction=$((100 - 100 * compressed / original))
                echo "  Original:  $original bytes"
                echo "  UPX:       $compressed bytes"
                echo "  Reduction: $reduction%"
              fi
            }
            
            # 🎪 Алиасы
            alias release="build-release"
            alias build-core="build-debug"
            alias run="./build/bin/goodnet"
            alias debug="gdb ./build/bin/goodnet"
            alias sizes="compare-sizes"
            
            echo "⚙️  Parallelism: $NPROC threads"
            echo "📋 Commands: release, sizes, run, debug"
          '';
        };
      }
    );
}
