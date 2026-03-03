# GoodNet — Логирование

## Обзор

GoodNet использует кастомный класс `Logger`, построенный поверх [spdlog](https://github.com/gabime/spdlog). Ключевые свойства:

- **Meyers Singleton** — ленивая инициализация, детерминированное разрушение
- **Форматирование с источником** — строки лога содержат относительный путь к файлу и номер строки
- **Debug/Release разделение** — `LOG_TRACE` и `LOG_DEBUG` компилируются в `((void)0)` в Release
- **Мост для плагинов** — плагины используют экземпляр логгера ядра без владения им

---

## Meyers Singleton

Экземпляр логгера хранится как `static` локальная переменная внутри `Logger::get_instance()`:

```cpp
// logger.cpp — единственный TU, который включает <spdlog/spdlog.h>
std::shared_ptr<spdlog::logger>& Logger::get_instance() noexcept {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}
```

**Почему это лучше глобального `static`-члена?**

Глобальный статический член инициализируется при старте программы в порядке, не определённом между единицами трансляции. Статическая локальная переменная инициализируется **при первом вызове** — безопасно, лениво и гарантированно потокобезопасно по стандарту C++11.

Кроме того, это решает **Static Destruction Order Fiasco** при завершении:

1. `Logger::shutdown()` вызывает `get_instance().reset()` → `instance = nullptr`
2. Позже, при выгрузке `libgoodnet_core.so`, `__do_global_dtors_aux` вызывает деструктор каждой статической локальной переменной библиотеки
3. Деструктор `shared_ptr` видит `nullptr` → `_M_release` проверяет `use_count` → пропускает `_M_dispose`
4. **Нет SIGSEGV**

Без явного `reset()` шаг 3 вызвал бы `_M_dispose` на уже уничтоженном объекте `spdlog::logger`.

**Чистота заголовков:** `<spdlog/spdlog.h>` включается **только в `logger.cpp`**. Публичный заголовок `logger.hpp` использует только `<spdlog/common.h>` (лёгкий) и forward declaration `spdlog::logger`. Каждый TU проекта, включающий `logger.hpp`, не тянет тяжёлую цепочку шаблонов spdlog.

---

## Конфигурация

Параметры логгера — публичные статические переменные класса `Logger`. Устанавливайте их **до первого вызова `LOG_*`** — логгер инициализируется лениво при первом использовании.

```cpp
// main.cpp — настройка до любого LOG_*
Logger::log_level = conf.get_or<std::string>("logging.level", "info");
Logger::log_file  = conf.get_or<std::string>("logging.file",  "logs/goodnet.log");
Logger::max_size  = static_cast<size_t>(conf.get_or<int>("logging.max_size", 10*1024*1024));
Logger::max_files = conf.get_or<int>("logging.max_files", 5);
```

### Ключи `config.json`

| Ключ | Тип | По умолчанию | Описание |
|---|---|---|---|
| `logging.level` | string | `"info"` | `trace` `debug` `info` `warn` `error` `critical` `off` |
| `logging.file` | string | `"logs/goodnet.log"` | Путь к файлу с ротацией |
| `logging.max_size` | int | `10485760` | Максимальный размер файла в байтах |
| `logging.max_files` | int | `5` | Количество хранимых ротаций |

### Режим детализации источника

`Logger::source_detail_mode` управляет форматом отображения места вызова:

| Режим | Вывод | Условие |
|---|---|---|
| `0` (авто) | `[src/main.cpp:42]` для debug/trace, `[main.cpp]` для info+ | по умолчанию |
| `1` (полный) | `[src/main.cpp:42]` | всегда |
| `2` (средний) | `[main.cpp:42]` | всегда |
| `3` (минимум) | `[main.cpp]` | всегда |

`Logger::project_root` обрезает абсолютный путь до относительного. Устанавливается автоматически из CMake-define `GOODNET_PROJECT_ROOT` (разворачивается в `CMAKE_SOURCE_DIR`), поэтому строки лога показывают `[src/config.cpp:42]` вместо полного пути из Nix store.

---

## Синки

При инициализации создаются два синка:

| Синк | Условие | Переменная паттерна |
|---|---|---|
| Файл с ротацией (`logs/goodnet.log`) | Всегда | `Logger::file_pattern` |
| Цветная консоль `stdout` | `#ifndef NDEBUG` (только Debug) | `Logger::console_pattern` |

Кастомный флаг `%Q` форматтера вставляет строку с источником (файл, опционально строка) в паттерн. Реализован целиком внутри `logger.cpp` и невидим для потребителей заголовка.

---

## Макросы логирования

### Всегда активны (info и выше)

```cpp
LOG_INFO("Сервер запущен на порту {}", port);
LOG_WARN("Приближается лимит соединений: {}/{}", current, max);
LOG_ERROR("Не удалось открыть файл: {}", path);
LOG_CRITICAL("Нет памяти, аварийное завершение");

INFO_VALUE(packet_count);      // LOG_INFO("packet_count = {}", packet_count)
ERROR_POINTER(handler_ptr);    // LOG_ERROR("handler_ptr [0x... valid:true]", ...)
```

### Только в Debug (в Release компилируются в ничто)

```cpp
LOG_DEBUG("Обработка пакета id={}", header->packet_id);
LOG_TRACE("Вход в цикл диспетчеризации");

DEBUG_VALUE(payload_size);          // LOG_DEBUG("payload_size = {}", payload_size)
TRACE_VALUE_DETAILED(conn_state);   // включает имя типа и sizeof

TRACE_POINTER(handler_ptr);         // адрес и признак валидности
DEBUG_POINTER(handler_ptr);

SCOPED_TRACE();   // логирует ">>> FunctionName" на входе, "<<< FunctionName" на выходе
SCOPED_DEBUG();   // то же, на уровне debug
```

В Release все debug-макросы разворачиваются в `((void)0)`. Нулевые накладные расходы — компилятор полностью удаляет их.

---

## Мост логирования для плагинов

Плагины загружаются с `dlopen(RTLD_LOCAL)`. Это означает, что каждое `.so` имеет **собственную копию** каждой статической переменной, включая `Logger::get_instance()`. По умолчанию эта копия — `nullptr`: первый вызов `LOG_*` в плагине разыменует null и упадёт.

Мост работает через `host_api_t::internal_logger`:

```
┌──────────────────────┐         ┌────────────────────────┐
│  libgoodnet_core.so  │         │  liblogger.so (плагин) │
│                      │         │                        │
│  Logger::get()  ─────┼──ptr───►│  api->internal_logger  │
│  (Meyers singleton)  │         │         │              │
│  spdlog::logger obj  │         │  sync_plugin_context() │
└──────────────────────┘         │         │              │
                                 │  shared_ptr<no-op del> │
                                 │         │              │
                                 │  Logger::get_instance()│
                                 │  = (заимствованный ptr)│
                                 └────────────────────────┘
```

В `main.cpp` перед загрузкой плагинов:
```cpp
api.internal_logger = static_cast<void*>(Logger::get().get());
```

В точке входа каждого плагина (через макрос `HANDLER_PLUGIN`):
```cpp
sync_plugin_context(api);
// разворачивается в:
Logger::set_external_logger(
    std::shared_ptr<spdlog::logger>(
        static_cast<spdlog::logger*>(api->internal_logger),
        [](spdlog::logger*) noexcept {}  // no-op deleter
    )
);
```

Копия `get_instance()` плагина теперь указывает на объект логгера ядра. **No-op deleter** гарантирует, что при вызове `dlclose()` деструктор `shared_ptr` плагина не вызовет `delete` на объекте, которым не владеет. Ядро управляет временем жизни логгера исключительно через `Logger::shutdown()`.
