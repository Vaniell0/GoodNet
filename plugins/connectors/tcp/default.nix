{ pkgs, mkCppPlugin, goodnetSdk, ... }:

mkCppPlugin {
  name        = "tcp";
  type        = "connectors";
  version     = "0.1.0";
  description = "High-performance TCP connector based on Boost.Asio";
  src         = ./.;
  deps        = [ pkgs.boost ];
  inherit goodnetSdk;
}
