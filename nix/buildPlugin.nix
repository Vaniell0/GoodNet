{ lib, stdenv, jq }:

{ name, type, version, description, drv }:

stdenv.mkDerivation {
  pname = "goodnet-${type}-${name}";
  inherit version;
  
  src = drv; # Это результат сборки CMake (rawBuild)

  nativeBuildInputs = [ jq ];

  installPhase = ''
    mkdir -p $out/lib

    # 1. Надежный поиск и копирование .so файлов
    # CMake install кладет их в $src/lib, но на всякий случай ищем везде
    echo "📦 Searching for shared objects in $src..."
    find $src -name "*.so" -exec cp -v {} $out/lib/ \;

    # Проверка, что файлы нашлись
    if [ -z "$(ls -A $out/lib)" ]; then
       echo "❌ Error: No .so files found in source!"
       exit 1
    fi

    # 2. Генерация упрощенного JSON манифеста
    echo "🔐 Signing plugin: ${name}..."
    
    for libfile in $out/lib/*.so; do
      filename=$(basename "$libfile")
      checksum=$(sha256sum "$libfile" | cut -d' ' -f1)
      
      # Cтруктура JSON
      ${jq}/bin/jq -n \
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
           integrity: {alg: "sha256", hash: $hash}
         }' > "$libfile.json"
         
      echo "   ✓ $filename -> JSON generated"
    done
  '';
}
