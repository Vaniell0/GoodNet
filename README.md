# GoodNet

**Модульная платформа для построения сетевых приложений на C++23**

> Альфа-версия · Архитектурно стабильна · Открыта для участия

[English documentation](docs/en/architecture.md) · [Документация на русском](docs/ru/architecture.md)

---

GoodNet — это не обёртка над Boost.Asio и не очередной HTTP-фреймворк.
Это **среда**, в которой инженер собирает сетевой узел из независимых блоков, не жертвуя контролем над транспортом, фильтрацией и безопасностью.

```
┌──────────────────────────────────────────────────────┐
│                goodnet  (узел / node)                │
│                                                      │
│   PacketSignal ──► Handler: logger                   │
│                ──► Handler: firewall                 │
│                ──► Handler: your_logic               │
│                                                      │
│   Connector: tcp    Connector: udp                   │
│   Connector: your_protocol                           │
└──────────────────────────────────────────────────────┘
```

---

## Философия

Большинство сетевых фреймворков навязывают модель «клиент–сервер» и прячут сокет за несколькими слоями абстракций. GoodNet исходит из другого принципа.

**Симметрия узлов.** В системе нет жёсткого деления на клиент и сервер. Каждый экземпляр приложения — это *узел (Node)*, который умеет одновременно принимать соединения и инициировать их. Логика определяется не ролью в топологии, а набором загруженных плагинов.

**Разделение транспорта и логики.** Connector отвечает исключительно за то, *как* байты попадают в узел. Handler — за то, *что* с ними происходит. Смена TCP на UDP или собственный протокол — это замена одного `.so` без изменения единой строки бизнес-логики.

**Прямой доступ.** Нет обёрток ради обёрток. Накладные расходы по сравнению с чистым Boost.Asio — минимальны. Параметры сокетов, таймауты, стратегии буферизации доступны напрямую в Connector-плагине.

**Конструктор, а не рамки.** Идея — «программная Cisco»: универсальный узел, который становится маршрутизатором, прокси, IoT-шлюзом или брокером сообщений в зависимости от набора загруженных модулей.

---

## Возможности

| Возможность | Описание |
|---|---|
| **Динамические плагины** | Handlers и Connectors загружаются как `.so` в рантайме |
| **Верификация плагинов** | Каждый `.so` проверяется по SHA-256-манифесту перед `dlopen` |
| **Асинхронная маршрутизация** | Пакеты доставляются через `PacketSignal` на Boost.Asio strand |
| **Симметрия узлов** | Нет разделения клиент/сервер на уровне архитектуры |
| **Изолированные плагины** | `RTLD_LOCAL` + `-fvisibility=hidden`: символы плагинов не пересекаются |
| **Детерминированный shutdown** | Порядок выгрузки зафиксирован: плагины → логгер. Нет SIGSEGV при завершении |
| **Воспроизводимая сборка** | Nix Flakes: одна команда — побайтово идентичный бинарник на любой машине |
| **Быстрая разработка** | `nix develop` + ccache + PCH: инкрементальная сборка за секунды |

---

## Архитектура

```
                      ┌────────────────────────┐
                      │  libgoodnet_core.so    │
                      │                        │
  config.json ──────► │  Config                │
                      │  Logger  (singleton)   │
                      │  PluginManager         │
                      │  PacketSignal (strand) │
                      └──────────┬─────────────┘
                                 │  dlopen(RTLD_LOCAL)
              ┌──────────────────┼─────────────────┐
              │                  │                 │
   ┌──────────▼──────┐  ┌────────▼───────┐  ┌──────▼──────────┐
   │ liblogger.so    │  │ libtcp.so      │  │ libcustom.so    │
   │ (Handler)       │  │ (Connector)    │  │ (ваш плагин)    │
   └─────────────────┘  └────────────────┘  └─────────────────┘
```

**Core** (`libgoodnet_core.so`) — разделяемая библиотека. Собирается как `SHARED` намеренно: это гарантирует единственный экземпляр статических переменных (прежде всего — Logger-синглтона) для ядра и всех плагинов одновременно.

**Handlers** получают пакет (`header_t` + payload) и делают с ним что угодно: пишут в файл, фильтруют, перенаправляют, анализируют. Узел может иметь произвольное число активных Handler-плагинов. Каждый подписывается на нужные типы пакетов через `supported_types`.

**Connectors** абстрагируют транспортный уровень. Коннектор умеет установить соединение (`create_connection`), слушать порт (`start_listening`) и идентифицируется строковой схемой (`get_scheme` → `"tcp"`, `"udp"`, `"ws"`). `PluginManager` находит нужный коннектор по схеме URI.

---

## Технологический стек

| Компонент | Детали |
|---|---|
| **Язык** | C++23 (GCC 15 / Clang 18+) |
| **Async I/O** | Boost.Asio 1.87 |
| **Сборка** | CMake 3.22+ · Ninja |
| **Воспроизводимость** | Nix Flakes (nixos-unstable) |
| **Логирование** | spdlog · fmt |
| **Конфигурация** | nlohmann/json |
| **Целостность плагинов** | libsodium (SHA-256) |

---

## Быстрый старт

### Требования

[Nix](https://nixos.org/download/) с включёнными экспериментальными функциями:

```bash
# ~/.config/nix/nix.conf
experimental-features = nix-command flakes
```

### Собрать и запустить

```bash
git clone https://github.com/your-org/goodnet
cd goodnet

nix build              # Release → ./result/bin/goodnet
./result/bin/goodnet

nix build .#debug      # Debug: символы + консольный лог
./result/bin/goodnet
```

### Среда разработки (инкрементальная сборка)

```bash
nix develop            # shell с cmake, ninja, ccache, GOODNET_SDK_PATH

cfgd                   # cmake configure для Debug (один раз)
bd                     # сборка — только изменённые файлы
./build/debug/goodnet
```

| Команда | Действие |
|---|---|
| `cfg` / `cfgd` | Configure Release / Debug |
| `b` / `bd` | Build (инкрементально) |
| `brun` / `bdrun` | Build + run |

### Собрать конкретный плагин

```bash
nix build .#plugins.handlers.logger
nix build .#plugins.connectors.tcp
```

---

## Написать свой плагин

Три файла — и плагин попадает в автоматическую сборку:

```cpp
// plugins/handlers/my_handler/my_handler.cpp
#include <handler.hpp>
#include <plugin.hpp>
#include <logger.hpp>

class MyHandler : public gn::IHandler {
public:
    void on_init() override {
        set_supported_types({MSG_TYPE_CHAT});
        LOG_INFO("MyHandler ready");
    }
    void handle_message(const header_t* h, const endpoint_t*,
                        const void* payload, size_t size) override {
        LOG_DEBUG("Packet id={} size={}", h->packet_id, size);
    }
    void on_shutdown() override { LOG_INFO("MyHandler stopped"); }
};

HANDLER_PLUGIN(MyHandler)
```

```cmake
# CMakeLists.txt
find_package(GoodNet REQUIRED)
include(${GOODNET_SDK_HELPER})
add_plugin(my_handler my_handler.cpp)
```

```nix
# default.nix
{ pkgs, mkCppPlugin, goodnetSdk, ... }:
mkCppPlugin {
  name = "my_handler";  type = "handlers";
  version = "1.0.0";    src = ./.; deps = [];
  inherit goodnetSdk;
}
```

Подробное руководство: [`docs/ru/plugins_development.md`](docs/ru/plugins_development.md)

---

## Документация

| | Русский | English |
|---|---|---|
| Архитектура | [docs/ru/architecture.md](docs/ru/architecture.md) | [docs/en/architecture.md](docs/en/architecture.md) |
| Разработка плагинов | [docs/ru/plugins_development.md](docs/ru/plugins_development.md) | [docs/en/plugins_development.md](docs/en/plugins_development.md) |
| Логирование | [docs/ru/logging.md](docs/ru/logging.md) | [docs/en/logging.md](docs/en/logging.md) |
| Справочник API | [docs/ru/api_reference.md](docs/ru/api_reference.md) | [docs/en/api_reference.md](docs/en/api_reference.md) |
| Система сборки | [docs/ru/build_system.md](docs/ru/build_system.md) | [docs/en/build_system.md](docs/en/build_system.md) |

---

## Статус и планы

**Текущий статус:** Alpha. Архитектура устоялась, публичный API может меняться.

**Что работает сейчас:**
- Загрузка и выгрузка Handler / Connector плагинов
- Верификация плагинов по SHA-256
- Асинхронная маршрутизация пакетов через Boost.Asio strand
- Логирование с мостом для плагинов (Meyers Singleton + no-op deleter)
- Воспроизводимая сборка через Nix Flakes

**Направления развития:**

| Направление | Описание |
|---|---|
| **IoT-шлюз** | Узел как брокер между MQTT, CoAP и TCP/IP-сетями |
| **Горячая перезагрузка** | Замена `.so` без остановки узла |
| **Маршрутизация по правилам** | DSL-фильтры для перенаправления пакетов |
| **TLS Connector** | Плагин на libsodium или OpenSSL |
| **Тесты** | Unit и интеграционные тесты для core и SDK |
| **Метрики** | Экспорт `gn::Stats` в Prometheus / InfluxDB |

---

## Участие в проекте

Проект открыт для идей и pull request'ов. Если вы хотите предложить новый тип Connector или изменение в SDK — откройте issue с описанием архитектурного контекста.

Перед отправкой кода:
- `nix build` проходит без ошибок
- `nix build .#debug` проходит без ошибок
- Новые плагины имеют корректный `default.nix` с полем `type =`
