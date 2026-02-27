{ pkgs, buildPlugin }:

{ name
, type
, src
, deps ? []
, description ? ""
, version ? "0.1.0"
, cmakeFlags ? []
, sign ? true
, goodnetSdk
}:

let
  rawBuild = pkgs.stdenv.mkDerivation {
    pname = "${name}-raw";
    inherit version src;

    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    buildInputs = deps ++ [ goodnetSdk ];

    cmakeFlags = cmakeFlags ++ [
      "-DGOODNET_SDK_PATH=${goodnetSdk}/sdk"
      "-DGOODNET_INC_PATH=${goodnetSdk}/include"
      "-GNinja"
    ];

    installPhase = ''
      mkdir -p $out/lib
      find . -name "*.so" -exec cp {} $out/lib/ \;
    '';
  };
in
  buildPlugin {
    inherit name type version description sign;
    drv = rawBuild;
  }
