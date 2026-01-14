#include "connector.hpp"
#include "plugin.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

// Используем пространство имен SDK
using namespace gn;

// ============================================================================
// 1. Реализация Соединения (Connection)
// Отвечает за конкретную сессию связи
// ============================================================================
class TcpConnection : public IConnection {
    std::string uri_;
    std::atomic<bool> connected_{false};

public:
    TcpConnection(const std::string& uri) : uri_(uri) {
        // Эмуляция подключения
        connected_ = true;
    }

    ~TcpConnection() {
        do_close();
    }

    // Отправка данных
    bool do_send(const void* data, size_t size) override {
        if (!connected_) return false;
        
        // TODO: Реализовать реальную отправку через сокеты (Boost.Asio или sys/socket.h)
        // Пока просто выводим в консоль для отладки, если бы это был дебаг
        return true;
    }

    // Закрытие соединения
    void do_close() override {
        bool expected = true;
        if (connected_.compare_exchange_strong(expected, false)) {
            // Реальное закрытие сокета должно быть тут
            notify_close(); 
        }
    }

    bool is_connected() const override {
        return connected_;
    }

    endpoint_t get_remote_endpoint() const override {
        // Заглушка. В реальности нужно брать IP из сокета.
        endpoint_t ep;
        memset(&ep, 0, sizeof(ep));
        return ep; 
    }

    std::string get_uri_string() const override {
        return uri_;
    }
    
    // Вызывается ядром при выгрузке или принудительной остановке
    void shutdown() override {
        do_close();
    }
};

// ============================================================================
// 2. Реализация Коннектора (Connector Factory)
// Отвечает за создание соединений и прослушивание портов
// ============================================================================
class TcpConnector : public IConnector {
public:
    TcpConnector() = default;
    ~TcpConnector() = default;

    // Фабричный метод: создает новое соединение
    std::unique_ptr<IConnection> create_connection(const std::string& uri) override {
        // Тут стоит добавить парсинг URI, чтобы убедиться, что это tcp://
        return std::make_unique<TcpConnection>(uri);
    }

    // Запуск сервера (Listen)
    bool start_listening(const std::string& host, uint16_t port) override {
        // TODO: Реализовать tcp::acceptor
        // Для примера вернем true
        return true; 
    }

    std::string get_scheme() const override {
        return "tcp";
    }

    std::string get_name() const override {
        return "Native TCP Connector";
    }

    void on_init() override {
        // Инициализация глобальных библиотек, если нужно
    }

    void on_shutdown() override {
        // Очистка ресурсов коннектора
    }
};

// ============================================================================
// 3. Регистрация Плагина
// ВАЖНО: Передаем класс КОННЕКТОРА (Фабрики), а не соединения!
// ============================================================================

CONNECTOR_PLUGIN(TcpConnector)