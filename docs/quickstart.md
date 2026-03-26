# Быстрый старт

Сборка и запуск GoodNet за 2 минуты.

См. также: [Сборка (подробно)](./build.md) · [Конфигурация](./config.md) · [Обзор архитектуры](./architecture.md)

## Сборка

### Nix (рекомендуется)

```bash
nix develop          # shell со всеми зависимостями
nix build            # сборка + прогон тестов
./result/bin/goodnet --help
```

Nix собирает три плагина как отдельные `.so` (TCP, ICE, Logger), подписывает их (SHA-256 manifest), и упаковывает всё в `result/`.

### CMake

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
./build/bin/unit_tests
./build/bin/goodnet --help
```

Зависимости: GCC 14+ или Clang 18+ (C++23), boost, libsodium, spdlog, fmt, nlohmann_json, zstd, GTest.

Подробнее: [Сборка](./build.md) — опции CMake, пресеты, Docker CI, почему SHARED.

## Запуск

### Сервер

```bash
./result/bin/goodnet --listen 25565
```

Вывод:
```
>>> Threads: 8  |  Packet: 64 KB  |  Target: (server only)
[Server 0m 02s] conns=0  RX 0.00 Gbps  TX 0.00 Gbps  0 pkt/s  auth✓0 ✗0  dec_fail 0  drops=0
```

Сервер принимает TCP (или ICE) соединения, автоматически выполняет [Noise_XX handshake](./protocol/noise-handshake.md), и слушает пакеты.

**Dashboard метрики:**
- `conns=N` — активные соединения
- `RX/TX Gbps` — throughput (приём/передача)
- `pkt/s` — пакетов в секунду
- `auth✓N ✗M` — успешные (✓) и неудачные (✗) аутентификации
- `dec_fail` — ошибки дешифрации ([ChaChaPoly-IETF AEAD](./protocol/crypto.md#aead-шифрование))
- `drops` — сброшенные пакеты ([backpressure, overflow](./architecture/signal-bus.md#stats))

### Клиент (бенчмарк)

```bash
# 64 KB пакеты, авто-потоки
./result/bin/goodnet --target tcp://192.168.1.100:25565

# 1 MB пакеты, 4 потока, 10000 пакетов
./result/bin/goodnet -t tcp://host:25565 --size 1024 -j 4 -n 10000

# ICE/DTLS (через NAT)
./result/bin/goodnet -t tcp://peer:25565 --ice-upgrade
```

Клиент ждёт handshake, потом запускает worker threads которые шлют пакеты на максимальной скорости. По завершении печатает итоги с histogram:

```
╔══════════════════════════════════════════════════════════╗
║                GoodNet Benchmark — ИТОГИ                 ║
╠══════════════════════════════════════════════════════════╣
║  Длительность : 5m 02s                                  ║
║  Статус : ЗАВЕРШЕНО (OK)                                 ║
╠══════════════════ ТРАФИК (Throughput) ═══════════════════╣
║  Среднее : 4.21 Gbps  ████████████████░░░░              ║
║  Пик : 5.03 Gbps                                        ║
║  Задержки (p95) : 3.87 Gbps                              ║
╠══════════════════ Histogram Gbps ════════════════════════╣
║    3.0- 3.3 Gbps │ ██                │   4%             ║
║    3.3- 3.5 Gbps │ ████              │   8%             ║
║    ...                                                   ║
╚══════════════════════════════════════════════════════════╝
```

### CLI флаги

| Флаг | Описание |
|------|----------|
| `-t, --target URI` | Целевой узел (`tcp://IP:PORT`) |
| `-l, --listen PORT` | Порт для входящих |
| `-j, --threads N` | IO потоки (0 = `hardware_concurrency`) |
| `-s, --size KB` | Размер пакета (default 64 KB) |
| `-n, --count N` | Лимит пакетов (0 = без лимита) |
| `-c, --config PATH` | JSON [конфигурация](./config.md) |
| `--hz RATE` | Частота dashboard (default 2 Hz) |
| `--ice-upgrade` | ICE/DTLS upgrade после TCP handshake |
| `--exit-after SEC` | Автостоп для CI |
| `--exit-code` | Код 1 при crypto ошибках (для CI) |
| `--no-color` | Без ANSI escape |

## Troubleshooting

### Connection hangs (соединение зависает)

**Симптомы:** Клиент запущен, но dashboard показывает `conns=0` дольше 30 секунд.

**Причины и решения:**
1. **Handshake timeout** — [Noise_XX](./protocol/noise-handshake.md) handshake завершается за 30s. Проверьте network latency:
   ```bash
   ping -c 5 <peer_ip>  # RTT должен быть < 1s
   ```
2. **Firewall блокирует порт** — проверьте `ufw` / `iptables`:
   ```bash
   sudo ufw allow 25565/tcp
   ```
3. **STUN недоступен** (при `--ice-upgrade`) — проверьте STUN сервер:
   ```bash
   nc -zv stun.l.google.com 19302  # Должен отвечать
   ```

### Crypto errors (выход с кодом 1 при `--exit-code`)

**Симптомы:** `auth✗N` растёт, `dec_fail > 0`, клиент завершается с exit code 1.

**Причины:**
1. **Key mismatch** — пиры используют разные [identity keys](./protocol/crypto.md#ключи). Проверьте:
   ```bash
   ls -la ~/.goodnet/  # user_key и device_key должны существовать
   ```
2. **Permissions на identity.dir** — процесс не может прочитать ключи:
   ```bash
   chmod 700 ~/.goodnet && chmod 600 ~/.goodnet/*_key
   ```
3. **[Cross-verification](./protocol/noise-handshake.md#cross-verification) failed** — device_pk не совпадает с Noise static key. Возможна подмена или ошибка деривации.

### Plugin not loading

**Симптомы:** Handler/connector не вызывается, логов загрузки нет.

**Проверки:**
1. **SHA-256 manifest** — файл `.so.json` существует и hash совпадает:
   ```bash
   sha256sum libmyhandler.so
   cat libmyhandler.so.json  # {"name": "...", "sha256": "..."}
   ```
2. **Приоритет GOODNET_PLUGINS_DIR** — env var перекрывает config:
   ```bash
   echo $GOODNET_PLUGINS_DIR  # Если set, проверьте содержимое
   ```
3. **Static plugins** — если собран с `GOODNET_STATIC_PLUGINS`, динамические .so игнорируются.

Подробнее: [Система плагинов](./architecture/plugin-system.md#загрузка-плагинов).

## Конфигурация

JSON с вложенными секциями:

```json
{
  "core": {
    "io_threads": 4
  },
  "logging": {
    "level": "debug",
    "file": "/var/log/goodnet.log"
  },
  "identity": {
    "dir": "~/.goodnet"
  },
  "plugins": {
    "base_dir": "/opt/goodnet/plugins"
  }
}
```

Полный справочник: [Конфигурация](./config.md).

## Минимальный plugin example

Простейший echo handler (10 строк) — отвечает тем же payload обратно:

```cpp
#include <handler.hpp>

class EchoHandler final : public gn::IHandler {
    const char* get_plugin_name() const override { return "echo"; }

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                        std::span<const uint8_t> payload) override {
        send_response(ep->peer_id, hdr->payload_type, payload);
    }

    propagation_t on_result(const header_t*, uint32_t) override {
        return PROPAGATION_CONSUMED;  // Захватить соединение
    }
};

HANDLER_PLUGIN(EchoHandler)
```

Компиляция:
```bash
g++ -std=c++23 -shared -fPIC -o libecho.so echo.cpp \
    -I../include -I../sdk/cpp -L../build -lgoodnet_core
sha256sum libecho.so | awk '{print "{\"name\":\"echo\",\"sha256\":\""$1"\"}"}' > libecho.so.json
```

Загрузка:
```bash
export GOODNET_PLUGINS_DIR=.
./goodnet --listen 25565  # Плагин загружается автоматически
```

Подробнее: [Handler: гайд](./guides/handler-guide.md).

## Программный API

### C++

```cpp
#include "core.hpp"
#include "config.hpp"

Config cfg(true);
cfg.plugins.base_dir = "./plugins";
cfg.core.io_threads = 4;

gn::Core core(&cfg);
core.run_async(4);  // 4 IO потока

// Подписка на тип сообщения
core.subscribe(100, "my_app", [](auto name, auto hdr, auto ep, auto data) {
    // data->data() — расшифрованный payload
    // ep->peer_id — conn_id для ответа
    return PROPAGATION_CONSUMED;
});

// Отправка
core.send("tcp://peer:25565", 100, payload.data(), payload.size());

// Завершение
core.stop();
```

### C API

```c
#include <core.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Создание core с default config
    gn_core_t* core = gn_core_create(NULL);
    if (!core) {
        fprintf(stderr, "Failed to create core\n");
        return 1;
    }

    // Запуск IO потоков
    if (gn_core_run_async(core, 4) != 0) {
        fprintf(stderr, "Failed to run core\n");
        gn_core_destroy(core);
        return 1;
    }

    // Подписка на тип сообщения
    gn_core_subscribe(core, 100, "my_handler", my_callback, 128, NULL);

    // Отправка
    const char* data = "Hello, GoodNet!";
    gn_core_send(core, "tcp://peer:25565", 100, data, strlen(data));

    // Проверка состояния
    if (gn_core_is_running(core)) {
        printf("Core is running, %u connections\n",
               gn_core_connection_count(core));
    }

    // Graceful shutdown
    gn_core_stop(core);
    gn_core_destroy(core);
    return 0;
}
```

C API (`src/capi.cpp`) — thin FFI bridge. Все функции NULL-safe и возвращают status codes.

**CAPI функции:**
- `gn_core_create(config)` — создать core (config может быть NULL для defaults)
- `gn_core_run_async(core, threads)` — запустить IO потоки
- `gn_core_is_running(core)` — проверить состояние
- `gn_core_connection_count(core)` — количество активных соединений
- `gn_core_stop(core)` — graceful shutdown
- `gn_core_destroy(core)` — освободить ресурсы

---

**См. также:** [Сборка](./build.md) · [Конфигурация](./config.md) · [Обзор архитектуры](./architecture.md) · [Handler: гайд](./guides/handler-guide.md)
