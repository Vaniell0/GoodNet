{ pkgs, mkCppPlugin, ... }:

mkCppPlugin {
  name = "logger";
  version = "1.0.0";
  description = "plugin for recording incoming messages";
  
  src = ./.;
  deps = [ pkgs.spdlog pkgs.fmt ];
}