# Build Tips & Troubleshooting

Практические советы по сборке и решение распространённых ошибок.

См. также: [Сборка](../build.md) · [Быстрый старт](../quickstart.md)

## Build troubleshooting

### Error: `-std=c++23: unrecognized`

**Причина:** Компилятор слишком старый (GCC < 14, Clang < 18).

**Решение:**

```bash
# Ubuntu: установить GCC 14
sudo apt-get install g++-14
export CXX=g++-14

# Или использовать Clang 18+
sudo apt-get install clang-18
export CXX=clang++-18

cmake -B build -DCMAKE_CXX_COMPILER=$CXX
```

**Проверка:**

```bash
$CXX --version
$CXX -std=c++23 -x c++ -E - <<< "int main(){}" > /dev/null && echo "C++23 OK"
```

### Error: `libsodium.so: undefined reference to ...`

**Причина:** Неполная libsodium installation или linking order неправильный.

**Решение:**

```bash
# Переустановить libsodium
sudo apt-get remove --purge libsodium-dev
sudo apt-get install libsodium-dev

# Проверить что pkg-config находит
pkg-config --libs libsodium
# Должен вывести: -lsodium

# Если нет → manual install:
wget https://download.libsodium.org/libsodium/releases/libsodium-1.0.19.tar.gz
tar xf libsodium-1.0.19.tar.gz && cd libsodium-1.0.19
./configure && make -j$(nproc) && sudo make install
sudo ldconfig
```

### Error: Logger singleton — duplicate symbol

**Причина:** Core собран как STATIC, но используются .so плагины → каждый плагин получает свою копию Logger singleton.

**Решение:**

```bash
# УБРАТЬ -DGOODNET_STATIC_CORE
cmake -B build  # default = SHARED
cmake --build build
```

**Почему:** Logger — Meyers singleton с `std::call_once`. STATIC library дублирует символы в каждом `.so` → два логгера.

### Error: Nix build — "file not found"

**Причина:** Новый файл не в git index.

**Решение:**

```bash
git add новый_файл.cpp
nix build  # теперь Nix увидит файл
```

Nix использует `builtins.fetchGit` → видит только tracked files.

### Error: Linking fails на macOS — undefined symbol `std::format`

**Причина:** Apple Clang не поддерживает `std::format` даже с `-std=c++23`.

**Решение:**

```bash
# Использовать Homebrew GCC 14
brew install gcc@14
export CXX=/opt/homebrew/bin/g++-14
cmake -B build -DCMAKE_CXX_COMPILER=$CXX
```

## Incremental build tips

### Partial rebuild (только tests)

```bash
# Собрать только unit_tests target (skip goodnet binary)
cmake --build build --target unit_tests -j$(nproc)

# Собрать только конкретный плагин
cmake --build build --target tcp_connector -j$(nproc)
```

### ccache integration

```bash
# Установить ccache
sudo apt-get install ccache

# Включить в CMake
cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Первая сборка: cold cache
cmake --build build -j$(nproc)  # ~45 секунд

# Вторая сборка (после touch одного файла): warm cache
touch src/core.cpp
cmake --build build -j$(nproc)  # ~2 секунды

# Статистика ccache
ccache -s
```

**Gain:** 20x speed improvement для incremental builds.

### Ninja вместо Make

```bash
# Ninja параллелизует лучше Make
cmake -B build -G Ninja
ninja -C build

# Rebuild одного файла:
touch src/core.cpp
ninja -C build  # ~1.5s (vs 3s с Make)
```

### Skip тестов при сборке

```bash
# Если меняете только core code (не тесты)
cmake -B build -DBUILD_TESTING=OFF
cmake --build build -j$(nproc)  # skip GTest compilation

# Запускать тесты вручную:
cmake -B build -DBUILD_TESTING=ON
cmake --build build --target unit_tests
./build/bin/unit_tests
```

---

**См. также:** [Сборка](../build.md) · [Config recipes](data/projects/GoodNet/docs/recipes/config-recipes.md)
