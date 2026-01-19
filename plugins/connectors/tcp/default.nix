{ pkgs, mkCppPlugin, ... }:

mkCppPlugin {
  name = "tcp";
  version = "2.0.0";
  description = "High-performance TCP connector based on Boost.Asio";
  
  src = ./.;
  deps = [ pkgs.boost ];
}