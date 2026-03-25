/// @file tests/mock_connector.cpp
/// @brief Mock connector plugin for PluginManager unit tests.
/// Scheme: "mock". Name: "MockConnector".
/// Compiled as libmock_connector.so.
/// Exports connector_init() via GN_EXPORT (CONNECTOR_PLUGIN macro).
///
/// do_send(), do_listen() are no-op stubs.
/// do_connect() always returns -1 (unsupported in mock).
/// Used only for testing plugin loading and connector lookup by scheme.
#include <connector.hpp>  // sdk/cpp/connector.hpp -> IConnector
#include <cstring>

class MockConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "mock"; }
    std::string get_name()   const override { return "MockConnector"; }

    void on_init() override {}

    int do_connect(const char* /*uri*/) override {
        return -1;
    }

    int do_listen(const char* /*host*/, uint16_t /*port*/) override {
        return 0;
    }

    int do_send(conn_id_t /*id*/, std::span<const uint8_t> /*data*/) override {
        return 0;
    }

    void do_close(conn_id_t /*id*/, bool /*hard*/) override {}

    void on_shutdown() override {}
};

CONNECTOR_PLUGIN(MockConnector)
