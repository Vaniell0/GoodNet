/// @file core/cm_capi.cpp
/// C ABI trampoline functions.

#include "connectionManager.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <sodium/crypto_sign.h>

namespace gn {

// ── Static trampolines ────────────────────────────────────────────────────────

conn_id_t ConnectionManager::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->handle_connect(ep); }
void ConnectionManager::s_on_data(void* ctx, conn_id_t id, const void* r, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->handle_data(id, r, sz); }
void ConnectionManager::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<ConnectionManager*>(ctx)->handle_disconnect(id, err); }
void ConnectionManager::s_send(void* ctx, const char* uri, uint32_t t,
                                const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send(uri, t, p, sz); }
void ConnectionManager::s_send_response(void* ctx, conn_id_t id, uint32_t t,
                                         const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send_on_conn(id, t, p, sz); }
void ConnectionManager::s_broadcast(void* ctx, uint32_t t, const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->broadcast(t, p, sz); }
void ConnectionManager::s_disconnect(void* ctx, conn_id_t id) {
    static_cast<ConnectionManager*>(ctx)->disconnect(id); }
int ConnectionManager::s_sign(void* ctx, const void* data, size_t sz, uint8_t sig[64]) {
    const auto* self = static_cast<ConnectionManager*>(ctx);
    std::shared_lock lk(self->identity_mu_);
    return crypto_sign_ed25519_detached(sig, nullptr,
        static_cast<const uint8_t*>(data), sz, self->identity_.device_seckey); }
int ConnectionManager::s_verify(void*, const void* data, size_t sz,
                                 const uint8_t* pk, const uint8_t* sig) {
    return crypto_sign_ed25519_verify_detached(
        sig, static_cast<const uint8_t*>(data), sz, pk); }
conn_id_t ConnectionManager::s_find_conn_by_pk(void* ctx, const char* hex) {
    return static_cast<ConnectionManager*>(ctx)->find_conn_by_pubkey(hex); }
int ConnectionManager::s_get_peer_info(void* ctx, conn_id_t id, endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->get_peer_endpoint(id, *ep) ? 0 : -1; }
int ConnectionManager::s_config_get(void* ctx, const char* key,
                                     char* buf, size_t sz) {
    auto* self = static_cast<ConnectionManager*>(ctx);
    if (!self->config_) return -1;
    auto v = self->config_->get<std::string>(key);
    if (!v) return -1;
    std::strncpy(buf, v->c_str(), sz - 1);
    buf[sz - 1] = '\0';
    return static_cast<int>(v->size());
}
void ConnectionManager::s_register_handler(void* ctx, handler_t* h) {
    static_cast<ConnectionManager*>(ctx)->register_handler(h); }
void ConnectionManager::s_log(void*, int level, const char* file, int line, const char* msg) {
    switch (level) {
        case 0: LOG_TRACE   ("{}", msg); break;
        case 1: LOG_DEBUG   ("{}", msg); break;
        case 2: LOG_INFO    ("{}", msg); break;
        case 3: LOG_WARN    ("{}", msg); break;
        case 4: LOG_ERROR   ("{}", msg); break;
        default:LOG_CRITICAL("{}", msg); break;
    }
}

} // namespace gn
