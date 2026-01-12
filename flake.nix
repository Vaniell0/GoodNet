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

        # 📚 Библиотеки (все зависимости для времени выполнения)
        libs = with pkgs; [ 
          fmt spdlog boost nlohmann_json libsodium
        ];

        # 🧩 SDK (заголовочные файлы для плагинов)
        sdk = pkgs.stdenv.mkDerivation {
          name = "goodnet-sdk";
          src = ./sdk;  # Источник - директория sdk/
          nativeBuildInputs = [ pkgs.cmake ];
          installPhase = ''
            mkdir -p $out/include/goodnet
            cp -r *.h cpp/*.hpp $out/include/goodnet/ 2>/dev/null || true
          '';
        };

        # 🔧 Универсальная функция сборки
        smartBuild = { name, src, extraFlags ? [] }: 
          pkgs.stdenv.mkDerivation {
            inherit name src;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
            buildInputs = libs;
            cmakeFlags = extraFlags;
            buildPhase = ''
              mkdir -p build && cd build
              cmake .. -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
                ${toString extraFlags}
              ninja
            '';
            installPhase = ''
              mkdir -p $out
            '';
          };

        # 🏗️ Основной пакет GoodNet
        goodnet = pkgs.stdenv.mkDerivation {
          pname = "goodnet";
          version = "0.1.0";
          src = self;  # Весь проект как источник

          nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
          buildInputs = libs;

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_PLUGINS=OFF"  # Плагины собираем отдельно
          ];

          installPhase = ''
            mkdir -p $out/bin
            find . -name goodnet -type f -executable -exec cp {} $out/bin/ \;
          '';
        };
      in
      {
        packages = {
          # 📦 Готовые пакеты для `nix build`
          default = goodnet;            # Основное приложение
          sdk = sdk;                    # SDK для разработки плагинов
          
          # 🔌 Плагины как отдельные пакеты
          plugin-handlers = smartBuild {
            name = "goodnet-handlers";
            src = ./plugins/handlers;
            extraFlags = [ "-DPLUGIN_BUILD=ON" ];
          };
          
          plugin-connectors = smartBuild {
            name = "goodnet-connectors";
            src = ./plugins/connectors;
            extraFlags = [ "-DPLUGIN_BUILD=ON" ];
          };
        };

        # 🛠️ Среда разработки
        devShells.default = pkgs.mkShell {
          name = "goodnet-dev";
          
          nativeBuildInputs = with pkgs; [ 
            cmake pkg-config gdb ninja gcc14 ccache
          ];
          
          buildInputs = libs;
          
          shellHook = ''
            echo "🚀 GoodNet Development Environment"
            
            # 🏎️ Кэш для ускорения компиляции
            export CCACHE_DIR="$HOME/.ccache/goodnet"
            mkdir -p "$CCACHE_DIR"
            
            # ⚙️ Автоматический расчёт параллелизма
            export NPROC=$(($(nproc) > 1 ? $(nproc) - 2 : 1))
            
            # 🔧 Функция умной сборки (из первого варианта)
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
            
            # 🎯 Специализированные команды (из второго варианта)
            build_core() {
              echo "🔨 Building core with $NPROC threads..."
              mkdir -p build
              pushd build > /dev/null
              cmake .. -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
                -DBUILD_PLUGINS=OFF
              ninja -j$NPROC
              popd > /dev/null
            }
            
            build_plugins() {
              echo "🔌 Building plugins..."
              smart_build "plugins/handlers" "-DPLUGIN_BUILD=ON"
              smart_build "plugins/connectors" "-DPLUGIN_BUILD=ON"
            }
            
            # 🎪 Полезные алиасы
            alias build-core="build_core"
            alias build-plugins="build_plugins"
            alias build-all="build_core && build_plugins"
            alias run="./build/bin/goodnet"
            alias debug="gdb ./build/bin/goodnet"
            
            echo "⚙️  Parallelism: $NPROC threads"
            echo "📋 Commands: build-core, build-plugins, build-all, run, debug"
          '';
        };
      }
    );
}