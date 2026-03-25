{ pkgs, mkCppPlugin, goodnetSdk, ... }:
mkCppPlugin {
  name        = "ice";
  type        = "connectors";
  version     = "0.2.0";
  description = "Lightweight ICE connector (Boost.Asio, RFC 8445)";
  src         = ./.;

  deps = with pkgs; [
    boost
    openssl
  ];

  inherit goodnetSdk;
}
