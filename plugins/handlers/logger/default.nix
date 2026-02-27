{ pkgs, mkCppPlugin, goodnetSdk }:

mkCppPlugin {
  name = "logger";
  type = "handlers";
  version = "0.1.0";
  description = "Message logging handler";
  src = ./.;
  deps = [ pkgs.spdlog ];
  goodnetSdk = goodnetSdk;
}
