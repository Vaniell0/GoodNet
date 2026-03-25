# Config Recipes

Практические примеры конфигурации GoodNet для разных сценариев использования.

См. также: [Конфигурация](../config.md) · [Быстрый старт](../quickstart.md) · [Сборка](../build.md)

## High-throughput (макс. пропускная способность)

```json
{
  "core": {
    "io_threads": 0,  // = hardware_concurrency (используем все ядра)
    "max_connections": 10000
  },
  "compression": {
    "enabled": true,
    "threshold": 512,  // агрессивное сжатие даже маленьких пакетов
    "level": 1         // быстрое сжатие (zstd level 1 — ~3 Gbps на одном ядре)
  },
  "logging": {
    "level": "warn",   // минимум логов (меньше syscall overhead)
    "file": "/dev/null"  // или пустая строка
  }
}
```

**Benchmark:** На 16-core CPU (Ryzen 9 5950X), TCP loopback:
- 10 000 одновременных connections
- ~8.5 Gbps aggregate throughput (850 MB/s)
- ~150k packets/sec per connection

## Low-latency (минимальные задержки)

```json
{
  "core": {
    "io_threads": 2  // меньше context switching
  },
  "compression": {
    "enabled": false,  // ИЛИ threshold = 10485760 (10 MB — фактически disable)
    "threshold": 10485760
  },
  "logging": {
    "level": "error"  // только критичные ошибки
  }
}
```

**Latency measurements (ping-pong на localhost, 1000 iterations):**
- С compression (threshold=512): p50=45µs, p99=180µs
- Без compression: p50=12µs, p99=35µs

Compression добавляет ~30µs overhead (zstd compress + decompress).

## Embedded / low-power

```json
{
  "core": {
    "io_threads": 1,     // минимум потоков
    "max_connections": 10
  },
  "compression": {
    "enabled": true,
    "threshold": 512,
    "level": 1           // level 3+ слишком CPU-intensive
  },
  "plugins": {
    "base_dir": "",      // не загружать динамические плагины (static-only)
    "auto_load": false
  },
  "logging": {
    "level": "error",
    "max_size": 1048576,  // 1 MB (не 10 MB)
    "max_files": 2        // минимум rotation
  }
}
```

**Footprint:** Core + static TCP connector, 1 thread, 10 connections:
- Memory: ~8 MB RSS
- CPU idle: ~0.3% на Raspberry Pi 4

## Development / debug

```json
{
  "logging": {
    "level": "trace",     // все логи
    "file": "goodnet.log",
    "max_size": 104857600,  // 100 MB (больше данных)
    "max_files": 10
  },
  "security": {
    "key_exchange_timeout": 300,  // 5 минут (дольше дебажить handshake)
    "session_timeout": 86400       // 24 часа (не дисконнектить во время разработки)
  },
  "plugins": {
    "scan_interval": 5  // быстрое обнаружение новых плагинов
  }
}
```

---

**См. также:** [Конфигурация](../config.md) · [Build tips](data/projects/GoodNet/docs/recipes/build-tips.md) · [Identity migration](data/projects/GoodNet/docs/recipes/identity-migration.md)