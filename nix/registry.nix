{ pkgs, buildPlugin, goodnetSdk }:

let
  lib = pkgs.lib;

  # Импорт плагина
  importPlugin = path: type: name:
    let
      pluginFunc = import path;
      plugin = pluginFunc {
        inherit pkgs;
        mkCppPlugin = args: (import ./mkCppPlugin.nix) {
          inherit pkgs buildPlugin goodnetSdk;
        } (args // { inherit type; });
      };
    in {
      inherit name type;
      drv = plugin;
    };

in rec {
  plugins = {
    handlers = {
      logger = importPlugin ../plugins/handlers/logger "handlers" "logger";
    };
    connectors = {
      tcp = importPlugin ../plugins/connectors/tcp "connectors" "tcp";
    };
  };

  allPlugins = (builtins.attrValues plugins.handlers) ++ (builtins.attrValues plugins.connectors);
  
  # Сборка бандла: собираем всё в одну папку
  makePluginBundle = { }:
    pkgs.runCommand "goodnet-plugins-bundle" {} ''
      mkdir -p $out/plugins/handlers
      mkdir -p $out/plugins/connectors
      
      echo "📚 Bundling plugins..."
      
      ${lib.concatMapStringsSep "\n" (p: ''
        echo "   -> Linking ${p.name} (${p.type})"
        # Копируем (или линкуем) файлы из пакета плагина в бандл
        # Важно: убрали '|| true', чтобы видеть ошибки
        ln -s ${p.drv}/lib/*.so $out/plugins/${p.type}/
        ln -s ${p.drv}/lib/*.json $out/plugins/${p.type}/
      '') allPlugins}
    '';
}
