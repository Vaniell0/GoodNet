#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>

namespace gn {

// ─── handle_data ──────────────────────────────────────────────────────────────
//
// TCP reassembly: накапливаем байты в recv_buf, вырезаем полные пакеты.
//
// Wire-формат пакета:
//   header_t (фиксированный размер)
//   payload  (header.payload_len байт)
//     → для ESTABLISHED non-localhost: nonce(8) + secretbox(plain)
//     → для ESTABLISHED localhost:     plain
//     → для AUTH:                      plain (auth_payload_t)

void ConnectionManager::handle_data(conn_id_t id, const void* raw, size_t size) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;

    std::unique_lock lock(records_mu_);
    auto it = records_.find(id);
    if (it == records_.end()) return;

    auto& rec = it->second;
    const auto* bytes = static_cast<const uint8_t*>(raw);
    rec.recv_buf.insert(rec.recv_buf.end(), bytes, bytes + size);

    while (rec.recv_buf.size() >= sizeof(header_t)) {
        const auto* hdr = reinterpret_cast<const header_t*>(rec.recv_buf.data());
        LOG_INFO("Incoming packet type={} from #{}", hdr->payload_type, id);

        if (hdr->magic != GNET_MAGIC) {
            LOG_WARN("handle_data #{}: bad magic 0x{:08X}, dropping", id, hdr->magic);
            rec.recv_buf.clear();
            break;
        }
        if (hdr->proto_ver != GNET_PROTO_VER) {
            LOG_WARN("handle_data #{}: bad proto_ver {}", id, hdr->proto_ver);
            rec.recv_buf.clear();
            break;
        }

        const size_t total = sizeof(header_t) + hdr->payload_len;
        if (rec.recv_buf.size() < total) break;  // ждём остаток

        // Вырезаем полный пакет из буфера
        std::vector<uint8_t> pkt(rec.recv_buf.begin(),
                                  rec.recv_buf.begin() + static_cast<ptrdiff_t>(total));
        rec.recv_buf.erase(rec.recv_buf.begin(),
                           rec.recv_buf.begin() + static_cast<ptrdiff_t>(total));

        const auto* phdr    = reinterpret_cast<const header_t*>(pkt.data());
        const auto* payload = pkt.data() + sizeof(header_t);
        const size_t plen   = phdr->payload_len;

        // Отпускаем lock перед dispatch — он может рекурсивно зайти в handle_data
        lock.unlock();
        dispatch_packet(id, phdr, payload, plen);
        lock.lock();

        it = records_.find(id);
        if (it == records_.end()) break;
    }
}

// ─── dispatch_packet ─────────────────────────────────────────────────────────
//
// AUTH    → process_auth() (plain, до сессии)
// прочие в AUTH_PENDING → drop (защита от preauth flood)
// прочие в ESTABLISHED:
//   localhost           → plain, прямо в bus_.emit
//   remote, sess ready  → decrypt(payload) → bus_.emit
//   remote, sess !ready → drop (не должно случиться в норме)

void ConnectionManager::dispatch_packet(conn_id_t id, const header_t* hdr,
                                         const uint8_t* payload, size_t plen) {
    if (hdr->payload_type == MSG_TYPE_AUTH) {
        process_auth(id, payload, plen);
        return;
    }

    // Читаем нужные поля под lock и сразу берём копию данных сессии
    endpoint_t remote{};
    bool       is_localhost  = false;
    bool       session_ready = false;

    // Для расшифровки нам нужен указатель на SessionState.
    // SessionState некопируем (atomic), поэтому берём shared_ptr-обёртку
    // через ConnectionRecord::session (unique_ptr).
    // Безопасный паттерн: расшифруем под отдельным lock.
    std::vector<uint8_t> plaintext;

    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        auto& rec = it->second;

        if (rec.state != STATE_ESTABLISHED) {
            LOG_WARN("dispatch #{}: type={} before ESTABLISHED, dropping",
                     id, hdr->payload_type);
            return;
        }

        remote       = rec.remote;
        is_localhost = rec.is_localhost;

        if (is_localhost || !rec.session) {
            // Plain — просто копируем payload
            plaintext.assign(payload, payload + plen);
        } else if (rec.session->ready) {
            session_ready = true;
            // Расшифровываем прямо под lock — session принадлежит record
            // и изменяется (nonce counter), поэтому нужен exclusive lock
            plaintext = rec.session->decrypt(payload, plen);
            if (plaintext.empty()) {
                LOG_WARN("dispatch #{}: decrypt failed (type={}), dropping",
                         id, hdr->payload_type);
                return;
            }
        } else {
            LOG_WARN("dispatch #{}: session not ready, dropping type={}", id, hdr->payload_type);
            return;
        }
    }
    (void)session_ready;  // только для документации

    auto hdr_ptr  = std::make_shared<header_t>(*hdr);
    auto data_ptr = std::make_shared<std::vector<uint8_t>>(std::move(plaintext));

    // emit за пределами records_mu_ — подписчики могут вызвать send()
    bus_.emit(hdr->payload_type, hdr_ptr, &remote, data_ptr);
}

} // namespace gn
