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

        # Общие зависимости для всего проекта
        commonDeps = with pkgs; [ 
          fmt boost nlohmann_json libsodium 
          cmake pkg-config gdb ninja gcc14
        ];

        # SDK как отдельный вывод
        sdk = pkgs.stdenv.mkDerivation {
          name = "goodnet-sdk";
          src = ./sdk;
          nativeBuildInputs = [ pkgs.cmake ];
          installPhase = ''
            mkdir -p $out/include/goodnet
            cp -r *.h cpp/*.hpp $out/include/goodnet/
          '';
        };

        # создание колекций плагинов
        makeCollection = { name, src, deps ? [] }:
          pkgs.stdenv.mkDerivation {
            inherit name src;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ] ++ deps;
            buildInputs = [ sdk ] ++ deps;
            
            cmakeFlags = [ "-DPLUGIN_BUILD=ON" ];

            # Nix соберет все .so, определенные в CMakeLists.txt этой папки
            installPhase = ''
              mkdir -p $out
            '';
          };

        # Создаем плагины
        handlersCollection = makeCollection {
          name = "goodnet-handlers";
          src = ./plugins/handlers;
        };

        connectorsCollection = makeCollection {
          name = "tcp-connector";
          src = ./plugins/connectors;
          deps = [ pkgs.boost ];
        };

      in
      {
        # Dev shell
        devShells.default = pkgs.mkShell {
          name = "goodnet-dev";
          
          nativeBuildInputs = with pkgs; [ 
            cmake pkg-config gdb ninja gcc14 
            ccache
          ];
          
          buildInputs = with pkgs; [
            fmt boost nlohmann_json libsodium
          ];
          
          shellHook = ''
            echo "🚀 GoodNet Development Environment (2025)"

            # Настройка ccache
            export CCACHE_DIR="$HOME/.ccache/goodnet"
            mkdir -p "$CCACHE_DIR"
            
            # Переменная для потоков (оставляем один поток свободным для системы)
            export NPROC=$(($(nproc) > 1 ? $(nproc) - 2 : 1))
            
            export LD_LIBRARY_PATH="$PWD/plugins/handlers/build/libs:$PWD/plugins/connectors/build/libs:$LD_LIBRARY_PATH"
            
            # Универсальная функция сборки через Ninja + ccache
            # $1 - путь к папке, $2 - флаги cmake
            smart_build() {
              local target_dir=$1
              local extra_flags=$2
              mkdir -p "$target_dir/build"
              pushd "$target_dir/build" > /dev/null
              cmake .. -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
                -DCMAKE_C_COMPILER_LAUNCHER=ccache \
                $extra_flags
              ninja -j$NPROC
              popd > /dev/null
            }

            build_core() {
              echo "🔨 Building core with $NPROC threads..."
              smart_build "." "-DBUILD_PLUGINS=OFF"
            }
            
            build_plugins() {
              echo "🔧 Building plugins with $NPROC threads..."
              echo "  [1/2] Handlers..."
              smart_build "plugins/handlers" "-DPLUGIN_BUILD=ON"
              echo "  [2/2] Connectors..."
              smart_build "plugins/connectors" "-DPLUGIN_BUILD=ON"
              
              # Копируем результат в общую папку билда для удобства запуска
              mkdir -p build/plugins/{handlers,connectors}
              # cp -f plugins/handlers/libs/*.so build/plugins/handlers/ 2>/dev/null || true
              # cp -f plugins/connectors/libs/*.so build/plugins/connectors/ 2>/dev/null || true
            }
            
            alias build-core="build_core"
            alias build-plugins="build_plugins"
            alias build-all="build_core && build_plugins"
            alias run="./build/bin/goodnet"
            alias debug="gdb ./build/bin/goodnet"

            echo "Parallelism set to: $NPROC threads"
            echo "Commands: build-core, build-plugins, build-all, run, debug"
          '';
        };
      }
    );
}
