{ pkgs, mkCppPlugin, goodnetSdk, ... }:

mkCppPlugin {
  name        = "logger";
  type        = "handlers";
  version     = "1.0.0";
  description = "Plugin for recording incoming messages";
  src         = ./.;
  deps        = [ ];
  inherit goodnetSdk;
}
