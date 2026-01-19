{ pkgs, buildPlugin, goodnetSdk }:

{ name, type, src, deps ? [], description ? "", version ? "0.1.0" }:

let
  rawBuild = pkgs.stdenv.mkDerivation {
    pname = "${name}-raw";
    inherit version src;
    
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
    buildInputs = deps ++ [ goodnetSdk ];
    
    cmakeFlags = [ 
        "-GNinja" 
        "-DCMAKE_BUILD_TYPE=Release"
    ];
  };

in
  buildPlugin {
    inherit name type version description;
    drv = rawBuild;
  }
