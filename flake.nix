{
  description = "GoodNet - Modular Network Application Framework";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        lib = pkgs.lib;

        # 1. Утилиты сборки
        buildPlugin = import ./nix/buildPlugin.nix { inherit lib pkgs; };
        mkCppPlugin = import ./nix/mkCppPlugin.nix { inherit pkgs buildPlugin; };

        # 2. Core / SDK
        goodnet-core = pkgs.stdenv.mkDerivation {
          pname = "goodnet-core";
          version = "0.1.0-alpha";
          src = ./.;
          nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
          buildInputs = with pkgs; [ boost spdlog libsodium fmt nlohmann_json ];
          cmakeFlags = [ "-DBUILD_AS_LIBRARY=ON" "-DINSTALL_DEVELOPMENT=ON" ];
        };

        # 3. Приложение
        goodnet-app = pkgs.stdenv.mkDerivation {
          pname = "goodnet-app";
          version = "0.1.0-alpha";
          src = ./.;
          nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
          buildInputs = with pkgs; [ boost spdlog libsodium fmt nlohmann_json ];
          cmakeFlags = [ "-DBUILD_AS_LIBRARY=OFF" ];
          installPhase = ''
            mkdir -p $out/bin
            cp bin/goodnet $out/bin/
          '';
        };

        # 4. Логика плагинов
        mapPlugins = type:
          let typeDir = ./plugins/${type}; in
          if lib.pathExists typeDir then
            lib.mapAttrs (name: _: 
              import (typeDir + "/${name}/default.nix") {
                inherit pkgs mkCppPlugin;
                goodnetSdk = goodnet-core;
              }
            ) (lib.filterAttrs (n: v: v == "directory" && lib.pathExists (typeDir + "/${n}/default.nix")) (builtins.readDir typeDir))
          else {};

        # Функция для создания "групп", которые можно и собирать целиком, и заходить внутрь
        makeGroup = name: attrs: 
          (pkgs.linkFarm name (lib.mapAttrsToList (n: v: { name = n; path = v; }) attrs)) // attrs;

        handlersMap = mapPlugins "handlers";
        connectorsMap = mapPlugins "connectors";

        pluginsTree = makeGroup "plugins-all" {
          handlers = makeGroup "handlers" handlersMap;
          connectors = makeGroup "connectors" connectorsMap;
        };

        # 5. Сборка бандлов (используем collect, чтобы достать все деривации из дерева)
        allPluginsList = lib.collect lib.isDerivation pluginsTree;

        pluginsBundle = pkgs.runCommand "goodnet-plugins-bundle" {} ''
          mkdir -p $out/plugins/{handlers,connectors}
          ${lib.concatMapStringsSep "\n" (p: ''
            if [[ "${p.pname}" == handlers-* ]]; then
              target="$out/plugins/handlers"
            else
              target="$out/plugins/connectors"
            fi
            cp -r ${p}/lib/*.so $target/ 2>/dev/null || true
            cp -r ${p}/lib/*.json $target/ 2>/dev/null || true
          '') allPluginsList}
        '';

        fullApp = pkgs.runCommand "goodnet-full" {} ''
          mkdir -p $out/{bin,plugins}
          cp -r ${goodnet-app}/bin/* $out/bin/
          cp -r ${pluginsBundle}/plugins/* $out/plugins/
          echo '{"core": {"log_level": "info"}, "plugins_dir": "./plugins"}' > $out/config.json
        '';

      in {
        packages = {
          default = fullApp;
          core = goodnet-core;
          app = goodnet-app;
          plugins = pluginsTree;
          bundle = pluginsBundle;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ goodnet-core ];
          packages = [ pkgs.jq pkgs.cmake-format ];
          shellHook = ''
            export GOODNET_SDK_PATH="${goodnet-core}/sdk"
          '';
        };
      });
}
