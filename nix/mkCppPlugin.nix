{ pkgs, buildPlugin }:

{ name, type, src, deps ? [], description ? "", version, cmakeFlags ? [], goodnetSdk }:

let
  rawBuild = pkgs.stdenv.mkDerivation {
    pname = "${name}-raw";
    inherit version src;

    nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
    buildInputs = deps ++ [ goodnetSdk ];

    cmakeFlags = cmakeFlags ++ [
      "-DCMAKE_PREFIX_PATH=${goodnetSdk}"
      "-DBUILD_SHARED_LIBS=ON"
    ];

    installPhase = ''
      mkdir -p $out/lib
      find . -name "*.so" -exec cp {} $out/lib/ \;
    '';
  };
in
  buildPlugin {
    inherit name type version description;
    drv = rawBuild;
  }
