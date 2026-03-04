# Тестирование GoodNet

## Обзор

GoodNet использует **GoogleTest / GoogleMock** для unit-тестов.
Тесты разделены на три группы:

| Файл | Что тестирует |
|------|---------------|
| `tests/conf.cpp` | `Config` — загрузка, сохранение, типы, дефолты |
| `tests/plugins.cpp` | `PluginManager` — загрузка плагинов, состояние, SHA-256 |
| `tests/connection_manager_test.cpp` | `ConnectionManager` — AUTH, сборка пакетов, PacketSignal, крипто |

---

## Запуск тестов

### Через Nix (рекомендуется)

```bash
# Собрать и запустить все тесты
nix build .#goodnet-core --extra-experimental-features 'nix-command flakes'

# Или внутри devShell
nix develop
cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -V
```

### Отдельный тест

```bash
# Запустить только тесты ConnectionManager
./build/debug/bin/unit_tests --gtest_filter='ConnMgrTest.*'

# Только конкретный тест
./build/debug/bin/unit_tests --gtest_filter='ConnMgrTest.ValidAuthTransitionsToEstablished'

# Все тесты с подробным выводом
./build/debug/bin/unit_tests --gtest_output=xml:test_results.xml
```

### Фильтры

```bash
# Все тесты PluginManager кроме потокобезопасности
./unit_tests --gtest_filter='PluginManagerTest.*-*ThreadSafe*'

# Только тесты AUTH
./unit_tests --gtest_filter='*Auth*'
```

---

## Архитектура тестов

### Изоляция через временные директории

Каждый тест получает собственную директорию в `/tmp/goodnet_*_<pid>` и удаляет её в `TearDown()`. Это гарантирует что тесты не мешают друг другу и не засоряют `~/.goodnet`.

```cpp
void SetUp() override {
    key_dir = fs::temp_directory_path() /
              fmt::format("goodnet_cm_test_{}", ::getpid());
    fs::create_directories(key_dir);
    // ...
}

void TearDown() override {
    fs::remove_all(key_dir);
}
```

### Мок-плагины

Тесты `PluginManager` используют два мок-плагина, которые собираются вместе с тестами:

**`tests/mock_handler.cpp`** — минимальный хендлер:
- `get_plugin_name()` → `"mock_handler"`
- Подписывается на `MSG_TYPE_SYSTEM` и `MSG_TYPE_CHAT`
- `handle_message()` логирует тип и размер

**`tests/mock_connector.cpp`** — коннектор с схемой `"mock"`:
- `create_connection()` → создаёт `MockConnection`, регистрирует через `register_connection()`
- `send_to()` → всегда возвращает `0` (успех), байты отбрасывает
- Полезен для тестирования маршрутизации без реального TCP

Каждый мок-плагин при установке получает манифест `.so.json` с SHA-256:

```cpp
fs::path install_mock(const char* src_path, const std::string& name) {
    // копируем .so во временную директорию
    // генерируем SHA-256 через libsodium
    // записываем <plugin>.so.json с integrity.hash
}
```

### Симуляция сетевых событий

Тесты `ConnectionManager` не поднимают реальный TCP. Вместо этого вызывают коллбэки `host_api_t` напрямую:

```cpp
// Симулировать входящее соединение
conn_id_t simulate_connect(addr, port) {
    host_api_t api{};
    cm->fill_host_api(&api);
    endpoint_t ep{addr, port};
    return api.on_connect(api.ctx, &ep);
}

// Симулировать поступление пакета
void simulate_data(conn_id_t id, uint32_t type, std::string payload) {
    // собираем wire-формат: header_t + payload
    api.on_data(api.ctx, id, wire.data(), wire.size());
}

// Симулировать AUTH от пира (с реальными Ed25519 ключами)
void simulate_auth_from_peer(conn_id_t id) {
    // генерируем keypair "пира"
    // строим auth_payload_t с корректной подписью
    // передаём как on_data
}
```

---

## Категории тестов

### NodeIdentity (ключи узла)

```
IdentityGeneratesOnFirstRun      — файлы user_key и device_key создаются
IdentityPersistsAcrossReloads    — повторная загрузка даёт тот же pubkey
IdentityPubkeyIsValidHex         — 64 hex-символа (32 байта)
UserAndDevicePubkeysAreDifferent — user != device ключ
```

### Управление соединениями

```
OnConnectReturnsValidId          — conn_id != CONN_ID_INVALID
OnConnectIncrementsCount         — connection_count() растёт
OnConnectIdsAreUnique            — все conn_id разные
OnConnectInitialStateIsAuthPending — первое состояние AUTH_PENDING
OnDisconnectRemovesRecord        — connection_count() уменьшается
OnDisconnectUnknownIdIsNoOp      — не падает на несуществующем id
```

### AUTH flow

```
ValidAuthTransitionsToEstablished  — корректный AUTH → STATE_ESTABLISHED
TamperedAuthSignatureRejected      — AUTH с плохой подписью → остаём AUTH_PENDING
BadMagicDropsBuffer                — неверный magic → буфер очищается, не крашится
```

### PacketSignal

```
EstablishedPacketReachesSignal   — пакет в ESTABLISHED → хендлер получает данные
PacketBeforeAuthIsDropped        — пакет в AUTH_PENDING → не доходит до хендлера
TcpFragmentationReassembly       — пакет разбитый на 3 части → одна доставка
MultiplePacketsInOneChunk        — два пакета в одном буфере → две доставки
```

### Криптография

```
SignAndVerifyRoundtrip            — sign + verify с правильным ключом = ok
TamperedSignatureFailsVerify      — изменённая подпись → verify != 0
WrongKeyFailsVerify               — верификация чужим ключом → != 0
```

### Многопоточность

```
ConcurrentConnectsAreThreadSafe  — 100 потоков параллельно on_connect
ConcurrentDataCallsAreThreadSafe — 50 потоков параллельно on_data в одно соединение
ConcurrentFindIsThreadSafe       — 20 потоков параллельно find_handler_by_name
ConcurrentEnableDisableIsThreadSafe — параллельные enable/disable
```

---

## Написание нового теста

### Тест PluginManager

```cpp
TEST_F(PluginManagerTest, MyNewTest) {
    // 1. Установить мок-плагин в нужную поддиректорию
    auto path = install_mock(MOCK_HANDLER_PATH, "my_plugin", "handlers");

    // 2. Загрузить
    auto res = pm->load_plugin(path);
    ASSERT_TRUE(res.has_value()) << res.error();

    // 3. Проверить
    auto h = pm->find_handler_by_name("mock_handler");
    ASSERT_TRUE(h.has_value());
    EXPECT_NE((*h)->handle_message, nullptr);
}
```

### Тест ConnectionManager

```cpp
TEST_F(ConnMgrTest, MyConnTest) {
    // 1. Подключить к PacketSignal
    std::atomic<int> count{0};
    sig->connect([&](auto hdr, auto ep, auto data) {
        count.fetch_add(1);
    });

    // 2. Симулировать соединение + AUTH
    conn_id_t id = simulate_connect("192.168.1.1", 8080);
    simulate_auth_from_peer(id);

    // 3. Проверить состояние
    EXPECT_EQ(*cm->get_state(id), STATE_ESTABLISHED);

    // 4. Отправить пакет, дождаться в PacketSignal
    simulate_data(id, MSG_TYPE_CHAT, "hello");
    for (int i = 0; i < 50 && count.load() == 0; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(count.load(), 1);
}
```

### Написание мок-плагина для теста

Если нужен кастомный плагин (например, с особым поведением `handle_message`):

```cpp
// tests/my_mock_plugin.cpp
#include <handler.hpp>
#include <plugin.hpp>
#include <atomic>

std::atomic<int> g_received_count{0};  // глобальный счётчик для теста

class CountingHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "counting"; }
    void on_init() override { set_supported_types({0}); }
    void handle_message(const header_t*, const endpoint_t*,
                         const void*, size_t) override {
        g_received_count.fetch_add(1);
    }
};

HANDLER_PLUGIN(CountingHandler)
```

В `CMakeLists.txt`:
```cmake
add_library(my_mock_plugin SHARED tests/my_mock_plugin.cpp)
target_link_libraries(my_mock_plugin PRIVATE goodnet_core)

target_compile_definitions(unit_tests PRIVATE
    MY_MOCK_PATH="$<TARGET_FILE:my_mock_plugin>"
)
```

---

## Инструменты анализа

### Thread Sanitizer (TSAN)

Ловит data race в многопоточных тестах:

```bash
cmake -B build/tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan
./build/tsan/bin/unit_tests --gtest_filter='*ThreadSafe*'
```

### Address Sanitizer (ASAN)

Ловит use-after-free, heap overflow, double-free:

```bash
cmake -B build/asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build/asan
./build/asan/bin/unit_tests
```

### Valgrind (альтернатива ASAN)

```bash
valgrind --leak-check=full --track-origins=yes \
    ./build/debug/bin/unit_tests 2>&1 | grep -E 'ERROR|LEAK|Invalid'
```

---

## Добавление тестов в CMakeLists.txt

```cmake
if(BUILD_TESTING AND GTest_FOUND)
    enable_testing()

    # Мок-плагины
    add_library(mock_handler   SHARED tests/mock_handler.cpp)
    add_library(mock_connector SHARED tests/mock_connector.cpp)
    target_link_libraries(mock_handler   PRIVATE goodnet_core)
    target_link_libraries(mock_connector PRIVATE goodnet_core)

    # Исполняемый файл тестов
    add_executable(unit_tests
        tests/conf.cpp
        tests/plugins.cpp
        tests/connection_manager_test.cpp   # ← добавить
    )

    target_compile_definitions(unit_tests PRIVATE
        MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
        MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
    )

    target_link_libraries(unit_tests PRIVATE
        goodnet_core
        GTest::gmock_main
        ${SODIUM_LIBRARIES}
    )

    add_test(NAME AllTests COMMAND unit_tests)
endif()
```

---

## Известные ограничения

**Тесты не проверяют:**
- Реальный TCP: сетевые тесты требуют поднятых сокетов — используй интеграционный запуск двух экземпляров `goodnet`
- X25519 key exchange: реализация в TODO — тесты появятся при реализации
- Шифрование payload: аналогично, пока plaintext

**Как тестировать end-to-end:**
```bash
# Терминал A
./result/bin/goodnet
goodnet> listen 11000

# Терминал B
./result/bin/goodnet
goodnet> connect 127.0.0.1 11000
goodnet> send 127.0.0.1 11000 Привет!
```
