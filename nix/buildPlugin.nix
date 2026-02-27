{ lib, pkgs }:

{ name, type, version, description ? "", drv, sign ? true }:

pkgs.stdenv.mkDerivation {
  pname = "${type}-${name}";
  inherit version;

  src = drv;

  nativeBuildInputs = lib.optional sign pkgs.jq;

  installPhase = ''
    mkdir -p $out/lib

    # Копируем все .so
    find $src -name "*.so" -exec cp {} $out/lib/ \;

    ${lib.optionalString sign ''
      echo "🔐 Signing plugin ${name}..."
      for libfile in $out/lib/*.so; do
        [ -e "$libfile" ] || continue
        checksum=$(sha256sum "$libfile" | cut -d' ' -f1)

        ${pkgs.jq}/bin/jq -n \
          --arg name "${name}" \
          --arg type "${type}" \
          --arg ver "${version}" \
          --arg desc "${description}" \
          --arg hash "$checksum" \
          '{
            meta: {
              name: $name,
              type: $type,
              version: $ver,
              description: $desc,
              timestamp: (now | todateiso8601)
            },
            integrity: { alg: "sha256", hash: $hash }
          }' > "$libfile.json"
      done
    ''}
  '';
}