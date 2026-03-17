# GoodNet

> **Симметричный фундамент для распределенных систем. Программная Cisco в каждом узле.**

GoodNet — это модульная сетевая операционная система (NetOS), заполняющая пустоту между низкоуровневыми сокетами (L4) и тяжелыми облачными протоколами (L7). Проект возвращает интернету изначальную симметрию узлов, делая создание безопасных P2P-соединений таким же простым, как написание REST API.

**📖 Обязательно к прочтению: [Манифест GoodNet: Четыреста миллиардов на посредничестве**](https://www.google.com/search?q=docs/manifesto.md) — системный анализ централизации интернета и архитектурной ниши, которую мы занимаем.

---

## 🎯 Проблема, которую мы решаем

Современные инструменты навязывают архитектуру:

* **gRPC / HTTP:** Жесткая асимметрия (клиент-сервер), внешний TLS, зависимость от сертификатов (CA).
* **ZeroMQ:** Паттерны зашиты в типы сокетов, шифрование опционально.
* **libp2p:** Тяжелые транзитивные зависимости, привязка к Web3, отсутствие стабильного C++ ядра.

**GoodNet занимает пустой квадрант:** прямой контроль над байтами, встроенная криптографическая аутентификация, полная симметрия узлов и модульность на базе C ABI.

---

## 🏗 Архитектура: Метафора Cisco

GoodNet спроектирован как аппаратное шасси маршрутизатора (например, Cisco 7500), но в программном виде:

1. **Core (Шасси):** Ядро на современном C++. Управляет асинхронным I/O, `SignalBus`, криптографическими ключами и очередями.
2. **Connector (Сетевая плата):** Плагины, отвечающие за транспорт (`tcp`, `udp`, `ice`). Реализуют интерфейс `connector_ops_t`.
3. **Handler (Прошивка логики):** Плагины, решающие бизнес-задачи (чат, телеметрия, DHT, роутинг). Реализуют интерфейс `handler_t`.

Граница между ядром и модулями — **стабильный C ABI** (`host_api_t`). Это позволяет писать плагины на любом языке, поддерживающем FFI (Rust, Go, Python), используя всю мощь C++ ядра.

Плагины могут загружаться двумя способами:
* **Динамически** (по умолчанию): `dlopen()`/`LoadLibrary()` — `.so`/`.dylib`/`.dll` файлы из директории плагинов.
* **Статически**: Исходники плагинов компилируются в бинарник, регистрация через `gn::static_plugin_registry()` — один бинарник без внешних зависимостей.

---

## 🔒 Криптография по умолчанию

В GoodNet нет понятия «опциональная безопасность».

* **Идентификация:** Публичный ключ (Ed25519) **является** адресом узла. Нет Удостоверяющих Центров (CA), нет DNS-зависимости.
* **Рукопожатие:** Noise-подобный протокол с идеальной прямой секретностью (PFS) на базе X25519 и BLAKE2b.
* **Пакеты:** Защищены AEAD (XSalsa20-Poly1305).

---

## 📦 Разработка плагинов (SDK)

SDK предоставляет zero-cost C++ обертки над C ABI для максимального удобства. Вся коммуникация между узлами строго типизирована (см. `messages.hpp`).

### Написание Handler-плагина

Для создания бизнес-логики достаточно унаследоваться от `gn::IHandler`:

```cpp
#include <sdk/cpp/handler.hpp>
#include <sdk/messages.hpp>

class MyPingHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "ping_handler"; }

    void on_init() override {
        // Подписываемся на пакеты типа HEARTBEAT
        set_supported_types({ gn::msg::MSG_TYPE_HEARTBEAT });
    }

    void handle_message(const gn::header_t* header,
                        const gn::endpoint_t* endpoint,
                        std::span<const uint8_t> payload) override 
    {
        // Zero-copy парсинг данных
        auto msg = gn::sdk::from_bytes<gn::msg::HeartbeatMessage>(
            payload.data(), payload.size()
        );

        if (msg->flags == 0x00) { // Это Ping
            msg->flags = 0x01;    // Делаем Pong
            // Отправляем ответ напрямую через peer_id
            send_response(endpoint->peer_id, gn::msg::MSG_TYPE_HEARTBEAT, msg.bytes(), msg.size());
        }
    }
};

// Экспорт — динамический или статический, определяется автоматически
HANDLER_PLUGIN(MyPingHandler)

```

### Написание Connector-плагина

Для нового транспорта — унаследоваться от `gn::IConnector`:

```cpp
#include <sdk/cpp/connector.hpp>

class MyTransport : public gn::IConnector {
public:
    const char* get_name() const override { return "my_transport"; }
    void on_init() override { /* настройка сокетов */ }
    int  listen(const char* addr) override { /* ... */ return 0; }
    int  connect(const char* addr) override { /* ... */ return 0; }
    int  send(peer_id_t peer, const struct iovec* iov, int iovcnt) override { /* ... */ return 0; }
    void shutdown() override { /* cleanup */ }
};

CONNECTOR_PLUGIN(MyTransport)
```

### Статическая линковка плагинов

По умолчанию плагины загружаются через `dlopen()` в рантайме. Для встраивания плагинов прямо в бинарник (удобно для Windows, портируемых сборок, embedded):

1. Скомпилируйте исходники плагинов вместе с вашим приложением
2. Добавьте `-DGOODNET_STATIC_PLUGINS` в `target_compile_definitions`
3. Вызовите `Core` как обычно — статические плагины зарегистрируются автоматически

```cmake
# CMakeLists.txt вашего приложения
add_executable(myapp
    src/main.cpp
    ${GOODNET_SRC_DIR}/plugins/connectors/tcp/tcp.cpp
    ${GOODNET_SRC_DIR}/plugins/handlers/logger/logger.cpp
)
target_compile_definitions(myapp PRIVATE GOODNET_STATIC_PLUGINS)
target_link_libraries(myapp PRIVATE goodnet::core)
```

Макросы `HANDLER_PLUGIN` / `CONNECTOR_PLUGIN` автоматически переключаются между `extern "C"` экспортом и регистрацией в `gn::static_plugin_registry()`. Динамическая загрузка `load_all_plugins()` и статическая `load_static_plugins()` совместимы — можно комбинировать оба подхода.

---

## 🚀 Сборка и Запуск

### Nix (Linux / macOS)

Проект использует **Nix Flakes** для 100% воспроизводимости окружения. Поддерживаются Linux (GCC) и macOS (Clang 18).

```bash
# Вход в окружение разработчика (со всеми зависимостями: cmake, boost, libsodium)
nix develop

# Быстрая сборка (Debug) и запуск
nix run

# Сборка и запуск Unit-тестов
nix run .#test

# Генерация HTML-отчета о покрытии кода тестами
nix run .#coverage

# Сборка Release-пакета (Core + Plugins bundle) для деплоя
nix build

# Docker-образ (только Linux)
nix build .#docker
```

### CMake (любая платформа)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
./build/bin/unit_tests
```

### Windows (экспериментально)

Для Windows рекомендуется статическая линковка плагинов (`GOODNET_STATIC_PLUGINS`), так как это избавляет от необходимости настраивать пути к `.dll` файлам. Зависимости через vcpkg:

```powershell
vcpkg install boost-asio boost-system libsodium zstd spdlog fmt nlohmann-json
cmake -B build -G "Visual Studio 17 2022" ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DBUILD_TESTING=ON
cmake --build build --config Release
```

> **Примечание:** ICE-коннектор использует `<sys/epoll.h>` и доступен только на Linux. TCP-коннектор полностью кроссплатформенный.

---

## 📡 Стандартные Wire-протоколы (L7)

GoodNet поставляется с набором готовых системных полезных нагрузок (Payloads) для быстрого старта:

* `AuthPayload`: Согласование ключей и обмен `CoreMeta` (ZSTD, ICE, Relay).
* `RelayPayload`: Gossip-маршрутизация и инкапсуляция.
* `DhtFindNodePayload` / `RouteAnnouncePayload`: Дискавери узлов и построение меш-топологии.
* `TunDataPayload`: Инкапсуляция сырых IP-пакетов для создания L3 VPN.

---

*GoodNet — Возвращаем интернет узлам.*
