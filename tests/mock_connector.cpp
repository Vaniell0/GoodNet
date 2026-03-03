#include <connector.hpp>
#include <plugin.hpp>
#include <cstring>

/**
 * @brief Minimal connection that always succeeds.
 */
class MockConnection : public gn::IConnection {
public:
    bool        do_send(const void*, size_t) override { return true; }
    void        do_close()                  override { notify_close(); }
    bool        is_connected() const        override { return true; }
    std::string get_uri_string() const      override { return "mock://test"; }

    endpoint_t get_remote_endpoint() const override {
        endpoint_t ep{};
        std::strncpy(ep.address, "127.0.0.1", sizeof(ep.address) - 1);
        ep.port = 0;
        return ep;
    }
};

/**
 * @brief Minimal connector for unit tests.
 *
 * Scheme: "mock". Name: "MockDevice".
 */
class MockConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "mock"; }
    std::string get_name()   const override { return "MockDevice"; }

    std::unique_ptr<gn::IConnection>
    create_connection(const std::string&) override {
        return std::make_unique<MockConnection>();
    }

    bool start_listening(const std::string&, uint16_t) override {
        return true;
    }
};

CONNECTOR_PLUGIN(MockConnector)
