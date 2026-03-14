{ pkgs, mkCppPlugin, goodnetSdk, ... }:
mkCppPlugin {
  name        = "ice";
  type        = "connectors";
  version     = "0.1.0";
  description = "ICE/DTLS connector based on libnice (RFC 5245 / RFC 8445)";
  src         = ./.;

  deps = with pkgs; [
    sysprof          # Необходим здесь как транзитивная зависимость glib
    libnice          # ICE agent + STUN/TURN
    glib             # GMainContext/GMainLoop required by libnice
  ];

  inherit goodnetSdk;
}