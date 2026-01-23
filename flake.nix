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
        
        commonArgs = {
          version = "1.0.0";
          src = ./.;
          nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
          buildInputs = with pkgs; [ boost spdlog libsodium fmt nlohmann_json ];
          cmakeFlags = [ "-GNinja" ];
        };

        # Пакет только с заголовками интерфейсов (SDK)
        goodnet-sdk = pkgs.stdenv.mkDerivation {
          pname = "goodnet-sdk";
          version = "1.0.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          cmakeFlags = [ "-DINSTALL_DEVELOPMENT=ON" "-DBUILD_AS_LIBRARY=OFF" ];
        };

        # Пакет с библиотекой реализации
        goodnet-core = pkgs.stdenv.mkDerivation (commonArgs // {
          pname = "goodnet-core";
          propagatedBuildInputs = with pkgs; [ 
            spdlog fmt boost 
            nlohmann_json 
            libsodium
          ];
          cmakeFlags = commonArgs.cmakeFlags ++ [ "-DBUILD_AS_LIBRARY=ON" "-DINSTALL_DEVELOPMENT=ON" ];
        });

        # Бинарник
        goodnet-app = pkgs.stdenv.mkDerivation (commonArgs // {
          pname = "goodnet";
          cmakeFlags = commonArgs.cmakeFlags ++ [ "-DBUILD_AS_LIBRARY=OFF" "-DINSTALL_DEVELOPMENT=OFF" ];
        });

        buildPlugin = import ./nix/buildPlugin.nix { inherit (pkgs) lib stdenv jq; };
        registry = import ./nix/registry.nix {
          inherit pkgs buildPlugin;
          goodnetSdk = goodnet-core;
        };

        pluginsBundle = registry.makePluginBundle {};

      in {
        packages = {
          core = goodnet-core;
          app = goodnet-app;
          
          # Удобный доступ к плагинам: nix build .#handlers.logger
          handlers = builtins.mapAttrs (n: v: v.drv) registry.plugins.handlers;
          connectors = builtins.mapAttrs (n: v: v.drv) registry.plugins.connectors;

          # Финальная сборка: всё в одном месте
          default = pkgs.symlinkJoin {
            name = "goodnet-full";
            paths = [ goodnet-app pluginsBundle ];
            
            postBuild = ''              
              echo "✅ Build complete. Structure:"
              ls -R $out/plugins
            '';
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/goodnet";
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ goodnet-core ];
          packages = [ pkgs.jq pkgs.cmakeCurses ];
        };
      });
}