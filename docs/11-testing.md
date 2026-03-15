# 11 — Тестирование

---

## Запуск

```bash
# Через CMake:
cd build && ctest --output-on-failure

# Напрямую:
./build/bin/unit_tests
./build/bin/unit_tests --gtest_filter="Core.*"
./build/bin/unit_tests --gtest_filter="ConnectionManager.*"
./build/bin/unit_tests --gtest_output="xml:results.xml"

# Через Nix:
nix run .#test                # Debug build + запуск
nix run .#coverage            # Coverage report (lcov → HTML)
```

---

## Структура тестов

| Файл | Тестирует | Кейсов |
|---|---|---|
| `tests/core.cpp` | Core lifecycle, singleton guard, C API, send, broadcast, stats | ~12 |
| `tests/conf.cpp` | Config: load/save, defaults, type mismatch, nested keys, edge cases | ~30 |
| `tests/plugins.cpp` | PluginManager: load, SHA-256, priority, enable/disable, concurrent query | ~12 |
| `tests/connection_manager.cpp` | CM: handshake, encrypt/decrypt, replay, relay, rekey, compression, routing | ~60 |
| `tests/signals.cpp` | SignalBus: subscribe, wildcard, priority, propagation, stats, concurrent | ~13 |
| `tests/queue_stress.cpp` | PerConnQueue: push/drain, backpressure, concurrent stress | ~7 |
| `tests/mock_handler.cpp` | Test fixture handler (.so) | — |
| `tests/mock_connector.cpp` | Test fixture connector (.so) | — |

Всего: **~168 тестов**.

---

## Ключевые тест-группы

### Core (tests/core.cpp)

```cpp
TEST(CoreTest, FastStartup)           // Core() + run_async + stop < timeout
TEST(CoreTest, SingletonGuard)        // второй Core() → exception
TEST(CoreTest, SequentialLifecycle)    // create → run → stop → create → run → stop
TEST(CoreTest, MultipleRunAsyncStopCycles)
TEST(CapiTest, LifecycleSanity)       // gn_core_create/run_async/stop/destroy
TEST(CapiTest, StatsInitiallyZero)    // gn_core_get_stats
TEST(CapiTest, SubscribeUnsubscribe)  // gn_core_subscribe/unsubscribe
TEST(CapiTest, NullCoreSafety)        // все функции с NULL → не крашатся
```

### Config (tests/conf.cpp)

```cpp
TEST(ConfigTest, DefaultsPopulatedOnConstruction)
TEST(ConfigTest, SetAndGet{Int,Bool,Double,String,Path})
TEST(ConfigTest, DottedKeyHierarchy)
TEST(ConfigTest, LoadValidJson)
TEST(ConfigTest, LoadOverridesDefaults)
TEST(ConfigTest, SaveAndReload)
TEST(ConfigTest, PathStoredInJsonReloadedAsString)  // fs::path ↔ string
TEST(ConfigDefaults, {Core,Logging,Security,Plugins}Values)
```

### ConnectionManager (tests/connection_manager.cpp)

```cpp
// Identity
TEST(IdentityTest, GeneratesFreshKeypair)
TEST(IdentityTest, LoadsPersistedKeypair)
TEST(IdentityTest, LoadsOpenSSHEd25519Key)
TEST(IdentityTest, DeviceKeyDependsOnUserKey)

// Session crypto
TEST(SessionTest, EncryptDecryptRoundTrip)
TEST(SessionTest, NoncesIncrement)
TEST(SessionTest, ReplayProtection)
TEST(SessionTest, TamperedPayloadRejected)
TEST(SessionTest, EmptyPlaintextRoundTrip)
TEST(SessionTest, LargePayloadRoundTrip)

// Compression
TEST(SessionCompressionTest, Encrypt_Large_UsesZstd)
TEST(SessionCompressionTest, Decrypt_Zstd_Decompresses)
TEST(SessionCompressionTest, CompressionRoundtrip_Stress)

// CM integration
TEST_F(CMTest, OnConnectReturnsValidId)
TEST_F(CMTest, OnConnectLocalhostFlaggedCorrectly)
TEST_F(CMTest, LocalhostHandshakeEstablishes)
TEST_F(CMTest, BadAuthPayload_{TooShort,InvalidSignature}_Dropped)
TEST_F(CMTest, PartialHeaderBufferedAndProcessedOnCompletion)
TEST_F(CMTest, TwoFramesInOneChunk)
TEST_F(CMTest, InvalidMagicDropped)
TEST_F(CMTest, HostApiSignVerifyRoundTrip)

// Relay
TEST_F(CMTest, HandleRelay_LocalDelivery)
TEST_F(CMTest, HandleRelay_Forward)
TEST_F(CMTest, HandleRelay_TTLZero_Dropped)
TEST_F(CMTest, HandleRelay_Dedup)

// Rekey
TEST_F(CMTest, RekeySession_SendsKeyExchange)
TEST_F(CMTest, RekeyResetsNonces)
TEST_F(CMTest, RekeyOnNonEstablished_Fails)

// Key derivation
TEST_F(CMTest, BothSides_IdenticalKey)
TEST_F(CMTest, DifferentEphemeral_DifferentKey)
TEST_F(CMTest, SortedPubkeys_Deterministic)
```

### SignalBus (tests/signals.cpp)

```cpp
TEST_F(SignalBusTest, SubscribeDispatch_TypeSpecific)
TEST_F(SignalBusTest, SubscribeDispatch_Wildcard)
TEST_F(SignalBusTest, PriorityOrdering)
TEST_F(SignalBusTest, PropagationConsumed_StopsChain)
TEST_F(SignalBusTest, PropagationReject)
TEST_F(SignalBusTest, Unsubscribe_Removes)
TEST_F(SignalBusTest, EmitStat_{RxTxBytes,Packets,Auth,ConnDisconn})
TEST_F(SignalBusTest, EmitDrop_AllReasons)
TEST_F(SignalBusTest, EmitLatency_Histogram)
TEST_F(SignalBusTest, ConcurrentSubscribeDispatch)
```

### PerConnQueue (tests/queue_stress.cpp)

```cpp
TEST(PerConnQueueTest, BasicPushAndDrain)
TEST(PerConnQueueTest, BackpressureLimitRespected)
TEST(PerConnQueueTest, ConcurrentPushDrain)            // N writers + 1 reader
TEST(PerConnQueueTest, ConcurrentPushBackpressureNoOverflow)
```

---

## Mock плагины

Mock handler и connector компилируются как `.so`:

```cmake
target_compile_definitions(unit_tests PRIVATE
    MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
    MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
)
```

Тесты получают пути через `#define` — никакого хардкода. Mock handler реализует `on_message_result()` для тестирования `PROPAGATION_CONSUMED`, `PROPAGATION_REJECT`, affinity.

---

## Coverage

```bash
nix run .#coverage
# Или вручную:
cmake -B build/coverage -DCMAKE_BUILD_TYPE=Debug -G Ninja \
      -DBUILD_TESTING=ON -DGOODNET_COVERAGE=ON
cmake --build build/coverage
lcov --zerocounters --directory build/coverage
./build/coverage/bin/unit_tests
lcov --capture --directory build/coverage -o coverage.info \
     --ignore-errors mismatch,inconsistent
lcov --remove coverage.info '/nix/*' '*/tests/*' '*/gtest/*' \
     '*/boost/*' '*/nlohmann/*' -o coverage_filtered.info \
     --ignore-errors unused,mismatch,inconsistent
genhtml coverage_filtered.info -o coverage_html --title "GoodNet Coverage"
```

HTML-отчёт: `build/coverage/coverage_html/index.html`

Текущее покрытие: ~75%.

---

## Как добавить тест

1. Определить, к какому файлу относится тест:
   - Core lifecycle → `tests/core.cpp`
   - Config → `tests/conf.cpp`
   - Plugin loading → `tests/plugins.cpp`
   - CM, crypto, framing → `tests/connection_manager.cpp`
   - SignalBus → `tests/signals.cpp`
   - Queue → `tests/queue_stress.cpp`

2. Добавить `TEST()` или `TEST_F()`:

```cpp
TEST(MyGroup, MyTestName) {
    // setup
    // action
    EXPECT_EQ(result, expected);
}
```

3. Для тестов CM используйте `CMTest` fixture — он создаёт CM с mock identity и SignalBus.

4. Если нужен новый mock плагин — создать `.cpp` в `tests/`, добавить `add_library(mock_X SHARED ...)` в CMakeLists.txt.

5. Запустить: `./build/bin/unit_tests --gtest_filter="MyGroup.*"`

---

*← [10 — Конфигурация](10-config.md) · [12 — Roadmap →](12-roadmap.md)*
