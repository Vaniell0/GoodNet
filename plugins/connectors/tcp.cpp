/*
 * tcp.cpp - TCP коннектор для GoodNet
 * 
 * ЭТОТ ФАЙЛ РЕАЛИЗУЕТ:
 * 1. TCP-соединения через Boost.Asio
 * 2. Поддержку URI формата: tcp://host:port
 * 3. Асинхронные операции ввода-вывода
 * 
 * КАК ЭТО РАБОТАЕТ:
 * 1. Ядро загружает этот плагин
 * 2. Находит функцию plugin_init() через dlsym()
 * 3. Вызывает её для получения connector_ops_t
 * 4. Использует операции из connector_ops_t для управления TCP соединениями
 */

#include "../sdk/cpp/connector.hpp"   // C++ обёртки для коннекторов
#include "../sdk/cpp/plugin.hpp"      // Макросы для экспорта плагинов
#include <boost/asio.hpp>             // Асинхронный ввод-вывод
#include <memory>                     // Умные указатели
#include <string>                     // Строки C++

// Используем пространство имён gn для наших C++ классов
using namespace gn;

/*
 * КЛАСС TcpConnection - ОПИСАНИЕ
 * 
 * Этот класс представляет одно TCP соединение. Он:
 * 1. Инкапсулирует Boost.Asio сокет
 * 2. Реализует интерфейс IConnection
 * 3. Управляет жизненным циклом TCP соединения
 * 4. Преобразует исключения Boost в коды ошибок для ядра
 * 
 * ПРИНЦИП РАБОТЫ:
 * - При создании получает io_context и URI
 * - Метод connect() устанавливает соединение
 * - Метод do_send() отправляет данные
 * - Метод do_close() закрывает соединение
 */
class TcpConnection : public IConnection {
private:
    // Boost.Asio сокет для TCP соединения
    boost::asio::ip::tcp::socket socket_;
    
    // URI соединения в строковом формате (например "tcp://192.168.1.1:8080")
    std::string uri_;
    
public:
    /*
     * КОНСТРУКТОР TcpConnection
     * 
     * ПАРАМЕТРЫ:
     * - io: ссылка на io_context (управляет асинхронными операциями)
     * - uri: строка URI для этого соединения
     * 
     * ВАЖНО: io_context должен жить дольше чем сокет!
     * Поэтому передаём его по ссылке, а не по значению.
     */
    TcpConnection(boost::asio::io_context& io, std::string uri)
        : socket_(io), uri_(std::move(uri)) 
    {
        // socket_ инициализирован через конструктор
        // uri_ перемещён (move) для эффективности
    }
    
    /*
     * МЕТОД do_send - ОТПРАВКА ДАННЫХ
     * 
     * ВЫПОЛНЯЕТ: синхронную отправку данных через TCP сокет
     * 
     * ПАРАМЕТРЫ:
     * - data: указатель на данные для отправки
     * - size: размер данных в байтах
     * 
     * ВОЗВРАЩАЕТ: true если отправка успешна, false при ошибке
     * 
     * ПРИМЕЧАНИЕ: Это блокирующая операция. В реальном приложении
     * нужно использовать асинхронную отправку boost::asio::async_write
     */
    bool do_send(const void* data, size_t size) override {
        try {
            // Используем boost::asio::write для гарантированной отправки всех данных
            boost::asio::write(socket_, boost::asio::buffer(data, size));
            return true;
        } 
        catch (const std::exception& e) {
            // Ошибка сети - уведомляем ядро
            notify_error(ECONNABORTED); // Используем стандартный код ошибки
            return false;
        }
        catch (...) {
            // Неизвестная ошибка
            notify_error(EIO); // Ошибка ввода-вывода
            return false;
        }
    }
    
    /*
     * МЕТОД do_close - ЗАКРЫТИЕ СОЕДИНЕНИЯ
     * 
     * ВЫПОЛНЯЕТ:
     * 1. Закрытие TCP сокета (если он открыт)
     * 2. Уведомление ядра через notify_close()
     * 3. Очистку состояния сокета
     */
    void do_close() override {
        // Проверяем открыт ли сокет перед закрытием
        if (socket_.is_open()) {
            try {
                // Закрываем сокет корректно
                socket_.close();
            }
            catch (...) {
                // Игнорируем ошибки при закрытии
            }
            
            // УВЕДОМЛЯЕМ ЯДРО что соединение закрыто
            notify_close();
        }
    }
    
    /*
     * МЕТОД is_connected - ПРОВЕРКА СОЕДИНЕНИЯ
     * 
     * ВОЗВРАЩАЕТ: true если TCP сокет открыт и соединение активно
     * 
     * ПРИМЕЧАНИЕ: Этот метод не проверяет работоспособность сети,
     * только состояние сокета. Для проверки соединения нужно
     * отправлять keep-alive пакеты.
     */
    bool is_connected() const override {
        return socket_.is_open();
    }
    
    /*
     * МЕТОД get_remote_endpoint - ИНФОРМАЦИЯ О КОНЕЧНОЙ ТОЧКЕ
     * 
     * ВОЗВРАЩАЕТ: endpoint_t структуру с информацией об удалённом хосте
     * 
     * ВАЖНО: Метод может вернуть пустую структуру если:
     * 1. Сокет не открыт
     * 2. Не удалось получить информацию от удалённого хоста
     */
    endpoint_t get_remote_endpoint() const override {
        endpoint_t ep = {}; // Инициализируем нулями
        
        try {
            // Проверяем открыт ли сокет
            if (socket_.is_open()) {
                // Получаем информацию об удалённой конечной точке
                auto remote = socket_.remote_endpoint();
                
                // Преобразуем IP адрес в строку
                std::string addr = remote.address().to_string();
                
                // КОПИРУЕМ СТРОКУ С ЗАЩИТОЙ ОТ ПЕРЕПОЛНЕНИЯ
                // sizeof(ep.address) даёт размер массива в байтах (128)
                strncpy(ep.address, addr.c_str(), sizeof(ep.address) - 1);
                ep.address[sizeof(ep.address) - 1] = '\0'; // Гарантируем null-terminated
                
                // Копируем порт
                ep.port = remote.port();
            }
        } 
        catch (...) {
            // В случае ошибки возвращаем пустую структуру
        }
        
        return ep;
    }
    
    /*
     * МЕТОД get_uri_string - ПОЛУЧЕНИЕ URI
     * 
     * ВОЗВРАЩАЕТ: строку URI в формате "tcp://host:port"
     * 
     * ПРИМЕЧАНИЕ: URI сохраняется при создании соединения и не изменяется
     */
    std::string get_uri_string() const override {
        return uri_;
    }
    
    /*
     * МЕТОД connect - УСТАНОВКА СОЕДИНЕНИЯ
     * 
     * ЭТО ВСПОМОГАТЕЛЬНЫЙ МЕТОД для использования внутри TcpConnector
     * 
     * ПАРАМЕТРЫ:
     * - host: имя хоста или IP адрес
     * - port: номер порта
     * 
     * ВОЗВРАЩАЕТ: true если соединение установлено успешно
     */
    bool connect(const std::string& host, uint16_t port) {
        try {
            // СОЗДАЁМ КОНЕЧНУЮ ТОЧКУ (адрес + порт)
            boost::asio::ip::tcp::endpoint endpoint(
                boost::asio::ip::make_address(host), // Преобразуем строку в IP адрес
                port                                   // Порт
            );
            
            // ВЫПОЛНЯЕМ СИНХРОННОЕ ПОДКЛЮЧЕНИЕ
            socket_.connect(endpoint);
            
            return true; // Успех!
        } 
        catch (const boost::system::system_error& e) {
            // Ошибка Boost.Asio - логируем код ошибки
            notify_error(e.code().value());
            return false;
        }
        catch (...) {
            // Любая другая ошибка
            notify_error(EIO);
            return false;
        }
    }
    
    /*
     * МЕТОД get_socket - ПОЛУЧЕНИЕ ССЫЛКИ НА СОКЕТ
     * 
     * ВОЗВРАЩАЕТ: ссылку на boost::asio::ip::tcp::socket
     * 
     * ПРИМЕЧАНИЕ: Этот метод используется для:
     * 1. Настройки параметров сокета (таймауты, буферы)
     * 2. Асинхронных операций чтения
     * 3. Получения дополнительной информации о соединении
     */
    boost::asio::ip::tcp::socket& get_socket() { 
        return socket_; 
    }
};

/*
 * КЛАСС TcpConnector - ОПИСАНИЕ
 * 
 * Этот класс представляет TCP коннектор. Он:
 * 1. Управляет пулом соединений
 * 2. Реализует интерфейс IConnector
 * 3. Создаёт экземпляры TcpConnection
 * 4. Предоставляет единый io_context для всех соединений
 * 
 * ПРИНЦИП РАБОТЫ:
 * - Инициализирует boost::asio::io_context при создании
 * - Создаёт новые TcpConnection при вызове create_connection()
 * - Управляет жизненным циклом io_context
 */
class TcpConnector : public IConnector {
private:
    // ОБЩИЙ КОНТЕКСТ ВВОДА-ВЫВОДА
    // Важно: все сокеты должны использовать один io_context
    std::shared_ptr<boost::asio::io_context> io_context_;
    
public:
    /*
     * КОНСТРУКТОР TcpConnector
     * 
     * ИНИЦИАЛИЗИРУЕТ: общий io_context для всех соединений
     * 
     * ВАЖНО: Используем shared_ptr чтобы гарантировать что
     * io_context живёт пока живёт хотя бы одно соединение
     */
    TcpConnector() 
        : io_context_(std::make_shared<boost::asio::io_context>()) 
    {
        // io_context создан, но пока не запущен
    }
    
    /*
     * МЕТОД on_init - ИНИЦИАЛИЗАЦИЯ КОННЕКТОРА
     * 
     * ВЫЗЫВАЕТСЯ: ядром после загрузки плагина
     * 
     * ВЫПОЛНЯЕТ: начальную настройку и логирование
     */
    void on_init() override {
        if (api_) {
            // Проверяем что мы действительно коннектор
            if (api_->plugin_type != PLUGIN_TYPE_CONNECTOR) {
                // Логируем ошибку
                return;
            }
            log("TCP connector initialized for connector type");
        }
    }
    
    /*
     * МЕТОД on_shutdown - ОСТАНОВКА КОННЕКТОРА
     * 
     * ВЫЗЫВАЕТСЯ: ядром при выгрузке плагина
     * 
     * ВЫПОЛНЯЕТ:
     * 1. Остановку io_context (прерывает все асинхронные операции)
     * 2. Освобождение ресурсов
     */
    void on_shutdown() override {
        if (io_context_) {
            // ОСТАНАВЛИВАЕМ io_context
            io_context_->stop();
            
            // ЛОГИРУЕМ остановку
            log("TCP connector stopped");
        }
    }
    
    /*
     * МЕТОД create_connection - СОЗДАНИЕ НОВОГО СОЕДИНЕНИЯ
     * 
     * ОСНОВНАЯ ФУНКЦИЯ коннектора - создание подключения к URI
     * 
     * ПАРАМЕТРЫ:
     * - uri: строка URI в формате "tcp://host:port"
     * 
     * ВОЗВРАЩАЕТ: unique_ptr<IConnection> или nullptr при ошибке
     * 
     * АЛГОРИТМ:
     * 1. Парсим URI на host и port
     * 2. Создаём TcpConnection
     * 3. Устанавливаем соединение
     * 4. Возвращаем объект соединения
     */
    std::unique_ptr<IConnection> create_connection(const std::string& uri) override {
        // ШАГ 1: ПРОВЕРЯЕМ ФОРМАТ URI
        // Ожидаем "tcp://host:port"
        size_t pos = uri.find("://");
        if (pos == std::string::npos) {
            log("ERROR: Invalid URI format - missing '://'");
            return nullptr;
        }
        
        // ШАГ 2: ИЗВЛЕКАЕМ HOST И PORT
        // uri = "tcp://host:port" → host_port = "host:port"
        std::string host_port = uri.substr(pos + 3);
        
        // Ищем разделитель host:port
        size_t colon = host_port.find(':');
        if (colon == std::string::npos) {
            log("ERROR: Invalid URI format - missing port");
            return nullptr;
        }
        
        // Разделяем на host и port
        std::string host = host_port.substr(0, colon);
        uint16_t port;
        
        try {
            // ПРЕОБРАЗУЕМ ПОРТ В ЧИСЛО
            port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
            
            // ПРОВЕРЯЕМ ДИАПАЗОН ПОРТА (1-65535)
            if (port == 0) {
                log("ERROR: Port cannot be 0");
                return nullptr;
            }
        } 
        catch (const std::invalid_argument&) {
            log("ERROR: Invalid port number - not a number");
            return nullptr;
        }
        catch (const std::out_of_range&) {
            log("ERROR: Port number out of range");
            return nullptr;
        }
        
        // ШАГ 3: СОЗДАЁМ СОЕДИНЕНИЕ
        auto connection = std::make_unique<TcpConnection>(*io_context_, uri);
        
        // ШАГ 4: УСТАНАВЛИВАЕМ СОЕДИНЕНИЕ
        if (connection->connect(host, port)) {
            // ЛОГИРУЕМ УСПЕШНОЕ СОЕДИНЕНИЕ
            log(("TCP connection established to " + host + ":" + std::to_string(port)).c_str());
            return connection;
        }
        
        // ШАГ 5: ОБРАБОТКА ОШИБКИ
        log("ERROR: TCP connection failed");
        return nullptr;
    }
    
    /*
     * МЕТОД start_listening - ЗАПУСК TCP СЕРВЕРА
     * 
     * ПРИМЕЧАНИЕ: В текущей реализации серверный режим не поддерживается
     * Этот метод должен быть реализован для полной функциональности TCP
     * 
     * ПЛАН РЕАЛИЗАЦИИ:
     * 1. Создать acceptor
     * 2. Начать асинхронное принятие соединений
     * 3. Для каждого нового соединения создавать TcpConnection
     * 4. Уведомлять ядро о новых входящих соединениях
     */
    bool start_listening(const std::string& host, uint16_t port) override {
        // TODO: Реализовать серверный режим
        log("WARNING: TCP listening not implemented yet");
        return false;
    }
    
    /*
     * МЕТОД get_scheme - ПОЛУЧЕНИЕ СХЕМЫ ПРОТОКОЛА
     * 
     * ВОЗВРАЩАЕТ: строку "tcp" - идентификатор протокола
     * 
     * ВАЖНО: Эта строка используется ядром для:
     * 1. Сопоставления URI с коннектором
     * 2. Определения какой коннектор использовать для "tcp://..."
     */
    std::string get_scheme() const override {
        return "tcp";
    }
    
    /*
     * МЕТОД get_name - ПОЛУЧЕНИЕ ИМЕНИ КОННЕКТОРА
     * 
     * ВОЗВРАЩАЕТ: человекочитаемое имя для отображения
     */
    std::string get_name() const override {
        return "TCP Connector";
    }
};

/*
 * МАКРОС ЭКСПОРТА ПЛАГИНА
 * 
 * CONNECTOR_PLUGIN(TcpConnector) создаёт:
 * 1. Функцию plugin_init() с C линкингом
 * 2. Статический экземпляр TcpConnector
 * 3. Преобразование C++ → C структур
 * 
 * КАК ЭТО РАБОТАЕТ:
 * 1. Ядро загружает libtcp.so
 * 2. Вызывает dlsym(handle, "plugin_init")
 * 3. Находит эту функцию
 * 4. Вызывает её для получения connector_ops_t
 * 5. Использует эти операции для работы с TCP
 */
CONNECTOR_PLUGIN(TcpConnector)

/* 
 * ИСПОЛЬЗОВАНИЕ:
 * Ядро будет автоматически загружать этот плагин при:
 * 1. Запуске приложения
 * 2. Обращении к URI "tcp://..."
 * 3. Явной загрузке через PluginManager
 */