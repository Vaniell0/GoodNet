# Testing GoodNet

## Overview

GoodNet uses **GoogleTest / GoogleMock** for unit testing.
Tests are split into three groups:

| File | Covers |
|------|--------|
| `tests/conf.cpp` | `Config` — load, save, types, defaults |
| `tests/plugins.cpp` | `PluginManager` — plugin loading, state, SHA-256 |
| `tests/connection_manager_test.cpp` | `ConnectionManager` — AUTH, packet assembly, PacketSignal, crypto |

---

## Running Tests

### Via Nix (recommended)

```bash
nix develop
cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -V
```

### Run a single test or suite

```bash
# All ConnectionManager tests
./build/debug/bin/unit_tests --gtest_filter='ConnMgrTest.*'

# One specific test
./build/debug/bin/unit_tests --gtest_filter='ConnMgrTest.ValidAuthTransitionsToEstablished'

# All tests with XML output
./build/debug/bin/unit_tests --gtest_output=xml:results.xml
```

### Filter patterns

```bash
# All PluginManager tests except thread-safety
./unit_tests --gtest_filter='PluginManagerTest.*-*ThreadSafe*'

# Everything related to AUTH
./unit_tests --gtest_filter='*Auth*'
```

---

## Test Architecture

### Isolation via temp directories

Each test fixture creates its own directory under `/tmp/goodnet_*_<pid>` and removes it in `TearDown()`. This prevents tests from interfering with each other and with the real `~/.goodnet`.

```cpp
void SetUp() override {
    key_dir = fs::temp_directory_path() /
              fmt::format("goodnet_cm_test_{}", ::getpid());
    fs::create_directories(key_dir);
}

void TearDown() override {
    fs::remove_all(key_dir);
}
```

### Mock plugins

Plugin tests use two mock shared libraries built alongside the test binary:

**`tests/mock_handler.cpp`** — minimal handler:
- `get_plugin_name()` → `"mock_handler"`
- Subscribes to `MSG_TYPE_SYSTEM` and `MSG_TYPE_CHAT`
- `handle_message()` logs type and size

**`tests/mock_connector.cpp`** — connector with scheme `"mock"`:
- `create_connection()` → creates a `MockConnection`, registers via `register_connection()`
- `send_to()` → always returns `0`, discards bytes
- Useful for testing routing without real TCP

Each mock plugin is installed with a `.so.json` manifest containing a SHA-256 hash:

```cpp
fs::path install_mock(const char* src_path, const std::string& name,
                      const fs::path& subdir = "") {
    // copy .so to temp dir
    // compute SHA-256 via libsodium
    // write <plugin>.so.json with integrity.hash
}
```

### Simulating network events

`ConnectionManager` tests never open real sockets. Instead they call `host_api_t` callbacks directly:

```cpp
// Simulate an incoming connection
conn_id_t simulate_connect(addr, port) {
    host_api_t api{};
    cm->fill_host_api(&api);
    endpoint_t ep{addr, port};
    return api.on_connect(api.ctx, &ep);
}

// Simulate incoming raw bytes
void simulate_data(conn_id_t id, uint32_t type, std::string payload) {
    // build wire: header_t + payload bytes
    api.on_data(api.ctx, id, wire.data(), wire.size());
}

// Simulate AUTH from a peer (with real Ed25519 keys)
void simulate_auth_from_peer(conn_id_t id) {
    // generate peer keypair
    // build auth_payload_t with valid signature
    // pass as on_data
}
```

---

## Test Categories

### NodeIdentity (node keys)

```
IdentityGeneratesOnFirstRun      — user_key and device_key files are created
IdentityPersistsAcrossReloads    — reloading the same dir yields the same pubkey
IdentityPubkeyIsValidHex         — 64 hex characters (32 bytes)
UserAndDevicePubkeysAreDifferent — user pubkey != device pubkey
```

### Connection management

```
OnConnectReturnsValidId          — conn_id != CONN_ID_INVALID
OnConnectIncrementsCount         — connection_count() grows
OnConnectIdsAreUnique            — all conn_ids are distinct
OnConnectInitialStateIsAuthPending — first state is AUTH_PENDING
OnDisconnectRemovesRecord        — connection_count() decreases
OnDisconnectUnknownIdIsNoOp      — doesn't crash on unknown id
```

### AUTH flow

```
ValidAuthTransitionsToEstablished  — correct AUTH → STATE_ESTABLISHED
TamperedAuthSignatureRejected      — AUTH with bad signature → stays AUTH_PENDING
BadMagicDropsBuffer                — wrong magic → buffer cleared, no crash
```

### PacketSignal routing

```
EstablishedPacketReachesSignal   — packet in ESTABLISHED → handler receives data
PacketBeforeAuthIsDropped        — packet in AUTH_PENDING → never reaches handler
TcpFragmentationReassembly       — packet split into 3 chunks → one delivery
MultiplePacketsInOneChunk        — two packets in one buffer → two deliveries
```

### Cryptography

```
SignAndVerifyRoundtrip            — sign + verify with correct key = ok
TamperedSignatureFailsVerify      — modified signature → verify != 0
WrongKeyFailsVerify               — verification with wrong pubkey → != 0
```

### Thread safety

```
ConcurrentConnectsAreThreadSafe  — 100 threads calling on_connect in parallel
ConcurrentDataCallsAreThreadSafe — 50 threads calling on_data on one connection
ConcurrentFindIsThreadSafe       — 20 threads calling find_handler_by_name
ConcurrentEnableDisableIsThreadSafe — parallel enable/disable calls
```

---

## Writing a New Test

### PluginManager test

```cpp
TEST_F(PluginManagerTest, MyNewTest) {
    // 1. Install the mock plugin in the appropriate subdirectory
    auto path = install_mock(MOCK_HANDLER_PATH, "my_plugin", "handlers");

    // 2. Load it
    auto res = pm->load_plugin(path);
    ASSERT_TRUE(res.has_value()) << res.error();

    // 3. Assert
    auto h = pm->find_handler_by_name("mock_handler");
    ASSERT_TRUE(h.has_value());
    EXPECT_NE((*h)->handle_message, nullptr);
}
```

### ConnectionManager test

```cpp
TEST_F(ConnMgrTest, MyConnTest) {
    // 1. Subscribe to PacketSignal
    std::atomic<int> count{0};
    sig->connect([&](auto hdr, auto ep, auto data) {
        count.fetch_add(1);
    });

    // 2. Simulate connection + AUTH
    conn_id_t id = simulate_connect("192.168.1.1", 8080);
    simulate_auth_from_peer(id);
    EXPECT_EQ(*cm->get_state(id), STATE_ESTABLISHED);

    // 3. Send a packet and wait for delivery
    simulate_data(id, MSG_TYPE_CHAT, "hello");
    for (int i = 0; i < 50 && count.load() == 0; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(count.load(), 1);
}
```

### Custom mock plugin

```cpp
// tests/counting_handler.cpp
#include <handler.hpp>
#include <plugin.hpp>
#include <atomic>

std::atomic<int> g_count{0};

class CountingHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "counting"; }
    void on_init() override { set_supported_types({0}); }
    void handle_message(const header_t*, const endpoint_t*,
                         const void*, size_t) override {
        g_count.fetch_add(1);
    }
};

HANDLER_PLUGIN(CountingHandler)
```

In `CMakeLists.txt`:
```cmake
add_library(counting_handler SHARED tests/counting_handler.cpp)
target_link_libraries(counting_handler PRIVATE goodnet_core)

target_compile_definitions(unit_tests PRIVATE
    COUNTING_HANDLER_PATH="$<TARGET_FILE:counting_handler>"
)
```

---

## Analysis Tools

### Thread Sanitizer (TSAN)

Catches data races in multithreaded tests:

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

Catches use-after-free, heap overflow, double-free:

```bash
cmake -B build/asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build/asan
./build/asan/bin/unit_tests
```

---

## Updating CMakeLists.txt

```cmake
if(BUILD_TESTING AND GTest_FOUND)
    enable_testing()

    add_library(mock_handler   SHARED tests/mock_handler.cpp)
    add_library(mock_connector SHARED tests/mock_connector.cpp)
    target_link_libraries(mock_handler   PRIVATE goodnet_core)
    target_link_libraries(mock_connector PRIVATE goodnet_core)

    add_executable(unit_tests
        tests/conf.cpp
        tests/plugins.cpp
        tests/connection_manager_test.cpp   # ← add this
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

## Known Limitations

**Tests do NOT cover:**
- Real TCP: network tests require two live `goodnet` processes — do this manually via the CLI
- X25519 key exchange: not yet implemented — tests will be added alongside the feature
- Payload encryption: same, currently plaintext

**End-to-end manual test:**
```bash
# Terminal A
./result/bin/goodnet
goodnet> listen 11000

# Terminal B
./result/bin/goodnet
goodnet> connect 127.0.0.1 11000
goodnet> send 127.0.0.1 11000 Hello from B!
```
