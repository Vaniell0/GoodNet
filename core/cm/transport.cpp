/// @file core/cm/transport.cpp
/// Send/recv framing, encryption (NoiseSession), compression, pending messages.

#include "impl.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstring>
#if !defined(_WIN32)
#include <sys/uio.h>
#endif

#include <sodium/crypto_aead_chacha20poly1305.h>
#include <sodium/utils.h>
#include <zstd.h>

#include "../sdk/connector.h"

namespace gn {

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════

bool ConnectionManager::Impl::is_localhost_address(std::string_view a) {
    return a == "127.0.0.1" || a == "::1" || a == "localhost" || a.starts_with("127.");
}

connector_ops_t* ConnectionManager::Impl::find_connector(const std::string& scheme) {
    std::shared_lock lk(connectors_mu_);
    auto it = connectors_.find(scheme.empty() ? "tcp" : scheme);
    return it != connectors_.end() ? it->second : nullptr;
}

std::optional<conn_id_t> ConnectionManager::Impl::resolve_uri(std::string_view uri) const {
    std::string key(uri);
    if (auto sep = key.find("://"); sep != std::string::npos)
        key = key.substr(sep + 3);
    std::shared_lock lk(uri_mu_);
    auto it = uri_index_.find(key);
    return it != uri_index_.end() ? std::optional{it->second} : std::nullopt;
}

std::vector<std::string> ConnectionManager::Impl::local_schemes() const {
    std::shared_lock lk(connectors_mu_);
    std::vector<std::string> out; out.reserve(connectors_.size());
    for (auto& [s, _] : connectors_) out.push_back(s);
    return out;
}

std::string ConnectionManager::Impl::negotiate_scheme(const ConnectionRecord& rec) const {
    const auto local = local_schemes();
    for (auto& prio : scheme_priority_) {
        if (std::find(local.begin(), local.end(), prio) == local.end()) continue;
        if (rec.peer_schemes.empty()) return prio;
        if (std::find(rec.peer_schemes.begin(), rec.peer_schemes.end(), prio)
                != rec.peer_schemes.end())
            return prio;
    }
    return local.empty() ? "tcp" : local.front();
}

std::shared_ptr<PerConnQueue> ConnectionManager::Impl::get_or_create_queue(conn_id_t id) {
    {
        std::shared_lock lk(queues_mu_);
        if (auto it = send_queues_.find(id); it != send_queues_.end())
            return it->second;
    }
    std::unique_lock lk(queues_mu_);
    auto& q = send_queues_[id];
    if (!q) q = std::make_shared<PerConnQueue>();
    return q;
}

uint64_t ConnectionManager::Impl::monotonic_ns() noexcept {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// ═══════════════════════════════════════════════════════════════════════════════
// NonceWindow
// ═══════════════════════════════════════════════════════════════════════════════

bool NonceWindow::accept(uint64_t nonce) {
    std::lock_guard lk(mu);
    if (nonce == 0) return false;

    if (highest_nonce == 0) {
        highest_nonce = nonce;
        bitmap.reset();
        bitmap.set(0);
        return true;
    }

    if (nonce > highest_nonce) {
        const uint64_t shift = nonce - highest_nonce;
        if (shift >= WINDOW_SIZE)
            bitmap.reset();
        else
            bitmap <<= static_cast<size_t>(shift);
        bitmap.set(0);
        highest_nonce = nonce;
        return true;
    }

    const uint64_t diff = highest_nonce - nonce;
    if (diff >= WINDOW_SIZE) return false;

    const auto idx = static_cast<size_t>(diff);
    if (bitmap.test(idx)) return false;
    bitmap.set(idx);
    return true;
}

void NonceWindow::reset() {
    std::lock_guard lk(mu);
    highest_nonce = 0;
    bitmap.reset();
}

// ═══════════════════════════════════════════════════════════════════════════════
// NoiseSession encrypt / decrypt
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr uint8_t FLAG_RAW  = 0x00;
static constexpr uint8_t FLAG_ZSTD = 0x01;

std::vector<uint8_t> NoiseSession::encrypt(const void* plain, size_t plain_len,
                                            uint64_t nonce,
                                            bool compress_enabled,
                                            int compress_threshold,
                                            int compress_level) {
    LOG_TRACE("encrypt: {} bytes, nonce={}", plain_len, nonce);
    bool compressed = false;
    std::vector<uint8_t> comp_buf;

    if (compress_enabled
        && plain_len > static_cast<size_t>(compress_threshold)) {
        const size_t bound = ZSTD_compressBound(plain_len);
        comp_buf.resize(bound);
        const size_t csize = ZSTD_compress(comp_buf.data(), bound,
                                            static_cast<const uint8_t*>(plain),
                                            plain_len, compress_level);
        if (!ZSTD_isError(csize) && csize < plain_len) {
            comp_buf.resize(csize);
            compressed = true;
        }
    }

    // Формируем body: flags + [orig_size] + data
    std::vector<uint8_t> body;
    if (compressed) {
        const uint32_t orig32 = static_cast<uint32_t>(plain_len);
        body.resize(1 + 4 + comp_buf.size());
        body[0] = FLAG_ZSTD;
        std::memcpy(body.data() + 1, &orig32, 4);
        std::memcpy(body.data() + 5, comp_buf.data(), comp_buf.size());
    } else {
        body.resize(1 + plain_len);
        body[0] = FLAG_RAW;
        if (plain_len && plain)
            std::memcpy(body.data() + 1, plain, plain_len);
    }

    // AEAD encrypt (ChaChaPoly IETF)
    constexpr size_t MAC_SIZE = crypto_aead_chacha20poly1305_IETF_ABYTES;
    std::vector<uint8_t> wire(body.size() + MAC_SIZE);

    uint8_t nonce12[12]{};
    std::memcpy(nonce12 + 4, &nonce, 8);

    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        wire.data(), &clen,
        body.data(), body.size(),
        nullptr, 0,
        nullptr, nonce12, send_key);

    wire.resize(static_cast<size_t>(clen));
    return wire;
}

std::vector<uint8_t> NoiseSession::decrypt(const void* wire_ptr, size_t wire_len,
                                            uint64_t nonce) {
    LOG_TRACE("decrypt: {} bytes, nonce={}", wire_len, nonce);
    constexpr size_t MAC_SIZE = crypto_aead_chacha20poly1305_IETF_ABYTES;
    if (wire_len < MAC_SIZE) {
        LOG_WARN("decrypt: too short ({} bytes)", wire_len);
        return {};
    }

    if (!recv_window.accept(nonce)) {
        LOG_WARN("decrypt: replay/out-of-window (nonce={})", nonce);
        return {};
    }

    uint8_t nonce12[12]{};
    std::memcpy(nonce12 + 4, &nonce, 8);

    const auto* wire = static_cast<const uint8_t*>(wire_ptr);
    std::vector<uint8_t> body(wire_len - MAC_SIZE);

    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            body.data(), &mlen, nullptr,
            wire, wire_len,
            nullptr, 0,
            nonce12, recv_key) != 0) {
        LOG_WARN("decrypt: AEAD MAC failed (nonce={})", nonce);
        return {};
    }

    body.resize(static_cast<size_t>(mlen));
    if (body.empty()) { LOG_WARN("decrypt: empty body"); return {}; }

    const uint8_t  flags   = body[0];
    const uint8_t* payload = body.data() + 1;
    const size_t   plen    = body.size() - 1;

    if (flags == FLAG_RAW)
        return std::vector<uint8_t>(payload, payload + plen);

    if (flags == FLAG_ZSTD) {
        if (plen < 4) { LOG_WARN("decrypt: no orig_size"); return {}; }
        uint32_t orig_size = 0;
        std::memcpy(&orig_size, payload, 4);
        if (!orig_size || orig_size > 128 * 1024 * 1024) {
            LOG_WARN("decrypt: implausible orig_size={}", orig_size);
            return {};
        }
        std::vector<uint8_t> plain(orig_size);
        const size_t dsize = ZSTD_decompress(plain.data(), orig_size,
                                              payload + 4, plen - 4);
        if (ZSTD_isError(dsize)) {
            LOG_WARN("decrypt: ZSTD error: {}", ZSTD_getErrorName(dsize));
            return {};
        }
        plain.resize(dsize);
        return plain;
    }

    LOG_WARN("decrypt: unknown flags 0x{:02X}", flags);
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════════
// build_frame / send_frame / flush
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> ConnectionManager::Impl::build_frame(conn_id_t id,
                                                            uint32_t msg_type,
                                                            std::span<const uint8_t> payload) {
    auto rec = rcu_find(id);
    if (!rec) return {};
    LOG_TRACE("build_frame #{}: type={} len={}", id, msg_type, payload.size());

    const bool is_handshake = (msg_type == MSG_TYPE_NOISE_INIT)
                           || (msg_type == MSG_TYPE_NOISE_RESP)
                           || (msg_type == MSG_TYPE_NOISE_FIN);

    // ── Localhost fast-path: без шифрования/сжатия ───────────────────────────
    if (rec->localhost_passthrough && !is_handshake) {
        header_t hdr{};
        hdr.magic        = GNET_MAGIC;
        hdr.proto_ver    = GNET_PROTO_VER;
        hdr.flags        = GNET_FLAG_TRUSTED;
        hdr.payload_type = static_cast<uint16_t>(msg_type);
        hdr.payload_len  = static_cast<uint32_t>(payload.size());
        hdr.packet_id    = rec->send_packet_id.fetch_add(1, std::memory_order_relaxed);

        std::vector<uint8_t> frame(sizeof(header_t) + payload.size());
        std::memcpy(frame.data(), &hdr, sizeof(header_t));
        if (!payload.empty())
            std::memcpy(frame.data() + sizeof(header_t),
                        payload.data(), payload.size());
        return frame;
    }

    // ── Standard path ────────────────────────────────────────────────────────
    thread_local std::vector<uint8_t> tl_enc;

    const uint64_t pkt_id = rec->send_packet_id.fetch_add(1, std::memory_order_relaxed);

    const bool do_encrypt = !is_handshake
                         && !rec->is_localhost
                         && rec->session;

    std::span<const uint8_t> final_payload = payload;

    if (do_encrypt) {
        const bool comp_en = config_ ? config_->compression.enabled   : true;
        const int  comp_th = config_ ? config_->compression.threshold : 512;
        const int  comp_lv = config_ ? config_->compression.level     : 1;
        tl_enc = rec->session->encrypt(payload.data(), payload.size(),
                                        pkt_id, comp_en, comp_th, comp_lv);
        final_payload = tl_enc;
    }

    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    if (rec->is_localhost)
        hdr.flags = GNET_FLAG_TRUSTED;
    hdr.payload_type = static_cast<uint16_t>(msg_type);
    hdr.payload_len  = static_cast<uint32_t>(final_payload.size());
    hdr.packet_id    = pkt_id;

    std::vector<uint8_t> frame(sizeof(header_t) + final_payload.size());
    std::memcpy(frame.data(), &hdr, sizeof(header_t));
    if (!final_payload.empty())
        std::memcpy(frame.data() + sizeof(header_t),
                    final_payload.data(), final_payload.size());
    return frame;
}

bool ConnectionManager::Impl::flush_frames_to_connector(
        conn_id_t id, connector_ops_t* ops,
        std::vector<std::vector<uint8_t>>& frames) {
    if (frames.empty()) return true;

    if (ops->send_gather && frames.size() > 1) {
        std::vector<struct iovec> iov;
        iov.reserve(frames.size());
        for (auto& f : frames)
            iov.push_back({f.data(), f.size()});

        const int rc = ops->send_gather(ops->connector_ctx, id,
                                         iov.data(), static_cast<int>(iov.size()));
        if (rc < 0) {
            LOG_ERROR("send_gather #{}: connector error", id);
            return false;
        }
        size_t total = 0;
        for (auto& f : frames) total += f.size();
        bus_.emit_stat({StatsEvent::Kind::TxBytes,  total, id});
        bus_.emit_stat({StatsEvent::Kind::TxPacket, (uint64_t)frames.size(), id});
        return true;
    } else {
        bool all_ok = true;
        for (auto& f : frames) {
            const int rc = ops->send_to(ops->connector_ctx, id,
                                         f.data(), f.size());
            if (rc == 0) {
                bus_.emit_stat({StatsEvent::Kind::TxBytes,  f.size(), id});
                bus_.emit_stat({StatsEvent::Kind::TxPacket, 1,        id});
            } else {
                LOG_ERROR("send_to #{}: connector error", id);
                all_ok = false;
            }
        }
        return all_ok;
    }
}

void ConnectionManager::Impl::send_frame(conn_id_t id, uint32_t msg_type,
                                          std::span<const uint8_t> payload) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;
    LOG_TRACE("send_frame #{}: type={} payload={}", id, msg_type, payload.size());
    auto frame = build_frame(id, msg_type, payload);
    if (frame.empty()) return;

    auto q = get_or_create_queue(id);
    if (!q->try_push(std::move(frame))) {
        bus_.emit_drop(id, DropReason::PerConnLimitExceeded);
        LOG_WARN("send_frame #{}: per-conn queue full", id);
        return;
    }
    flush_queue(id, *q);
}

void ConnectionManager::Impl::flush_queue(conn_id_t id, PerConnQueue& q) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;
    auto rec = rcu_find(id);
    if (!rec) return;

    auto batch = q.drain_batch(64);
    if (batch.empty()) return;

    // Собираем активные пути, отсортированные по приоритету
    std::vector<TransportPath*> paths;
    for (auto& tp : rec->transport_paths)
        if (tp.active) paths.push_back(&tp);
    std::sort(paths.begin(), paths.end(),
              [](const TransportPath* a, const TransportPath* b) {
                  return a->priority < b->priority;
              });

    // Пробуем каждый путь в порядке приоритета
    for (auto* path : paths) {
        auto* ops = find_connector(path->scheme);
        if (!ops) continue;
        if (flush_frames_to_connector(path->transport_conn_id, ops, batch)) {
            path->consecutive_errors = 0;
            return;
        }
        path->consecutive_errors++;
        if (path->consecutive_errors >= 3) {
            path->active = false;
            LOG_WARN("flush_queue #{}: path '{}' deactivated after {} consecutive errors",
                     id, path->scheme, path->consecutive_errors);
        }
    }

    // Fallback: если transport_paths пуст — старая логика
    if (paths.empty()) {
        const std::string& scheme = rec->negotiated_scheme.empty()
            ? rec->local_scheme : rec->negotiated_scheme;
        auto* ops = find_connector(scheme);
        if (ops) {
            flush_frames_to_connector(id, ops, batch);
            return;
        }
    }

    // Все пути отказали
    bus_.emit_drop(id, DropReason::ConnectorNotFound);
    LOG_WARN("flush_queue #{}: all transport paths failed", id);
}

// ═══════════════════════════════════════════════════════════════════════════════
// send / broadcast
// ═══════════════════════════════════════════════════════════════════════════════

bool ConnectionManager::Impl::send(std::string_view uri, uint32_t msg_type,
                                    std::span<const uint8_t> payload) {
    LOG_TRACE("send(uri): uri={} type={} len={}", uri, msg_type, payload.size());
    if (uri.empty() || shutting_down_.load(std::memory_order_relaxed)) return false;

    const std::string uri_str(uri);
    const auto sep = uri_str.find("://");
    const std::string scheme = (sep != std::string::npos)
                             ? uri_str.substr(0, sep) : "tcp";
    std::string uri_key = (sep != std::string::npos) ? uri_str.substr(sep + 3) : uri_str;

    auto conn_id_opt = resolve_uri(std::string_view(uri));
    if (!conn_id_opt) {
        // Авто-соединение: queue message и инициируем connect
        {
            std::unique_lock lk(pending_mu_);
            auto& queue = pending_messages_[uri_key];

            if (queue.size() >= PENDING_MAX_PER_URI) {
                LOG_WARN("Pending queue full for URI: {}", uri_key);
                return false;
            }

            queue.emplace_back(msg_type, std::vector<uint8_t>(payload.begin(), payload.end()));
            LOG_DEBUG("Queued message type={} for pending URI: {} (queue size: {})",
                      msg_type, uri_key, queue.size());
        }

        if (auto* ops = find_connector(scheme)) {
            ops->connect(ops->connector_ctx, uri_str.c_str());
        } else {
            LOG_WARN("No connector for scheme '{}', message queued but cannot connect", scheme);
        }

        return true;
    }

    const conn_id_t id = *conn_id_opt;
    auto rec = rcu_find(id);
    if (!rec) return false;

    const bool is_handshake = (msg_type == MSG_TYPE_NOISE_INIT)
                           || (msg_type == MSG_TYPE_NOISE_RESP)
                           || (msg_type == MSG_TYPE_NOISE_FIN);

    if (rec->state != STATE_ESTABLISHED && !is_handshake) {
        std::unique_lock lk(pending_mu_);
        auto& queue = pending_messages_[uri_key];

        if (queue.size() >= PENDING_MAX_PER_URI) {
            LOG_WARN("Pending queue full for connecting URI: {}", uri_key);
            return false;
        }

        queue.emplace_back(msg_type, std::vector<uint8_t>(payload.begin(), payload.end()));
        LOG_DEBUG("Queued message type={} for connecting URI: {} (state={})",
                  msg_type, uri_key, static_cast<int>(rec->state));
        return true;
    }

    const size_t global_pending = get_pending_bytes();
    if (global_pending + payload.size() > GLOBAL_MAX_IN_FLIGHT) {
        bus_.emit_stat({StatsEvent::Kind::Backpressure, 1, id});
        return false;
    }

    if (payload.size() > CHUNK_SIZE * 2) {
        size_t offset = 0;
        while (offset < payload.size()) {
            const size_t chunk = std::min(CHUNK_SIZE, payload.size() - offset);
            send_frame(id, msg_type, payload.subspan(offset, chunk));
            offset += chunk;
        }
    } else {
        send_frame(id, msg_type, payload);
    }
    return true;
}

bool ConnectionManager::Impl::send(conn_id_t id, uint32_t msg_type,
                                    std::span<const uint8_t> payload) {
    LOG_TRACE("send(id): #{} type={} len={}", id, msg_type, payload.size());
    if (!rcu_find(id)) return false;
    send_frame(id, msg_type, payload);
    return true;
}

void ConnectionManager::Impl::broadcast(uint32_t msg_type,
                                         std::span<const uint8_t> payload) {
    LOG_TRACE("broadcast: type={} len={}", msg_type, payload.size());
    auto map = rcu_read();
    for (auto& [id, rec] : *map) {
        if (rec->state == STATE_ESTABLISHED)
            send_frame(id, msg_type, payload);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pending messages
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::flush_pending_messages(const std::string& uri,
                                                      conn_id_t id) {
    std::vector<PendingMessage> messages;

    {
        std::unique_lock lk(pending_mu_);
        auto it = pending_messages_.find(uri);
        if (it == pending_messages_.end() || it->second.empty())
            return;

        messages = std::move(it->second);
        pending_messages_.erase(it);
    }

    LOG_INFO("Flushing {} pending messages for URI: {}", messages.size(), uri);

    for (auto& msg : messages) {
        send_frame(id, msg.msg_type, std::span<const uint8_t>(msg.payload));
    }
}

void ConnectionManager::Impl::cleanup_stale_pending() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> stale_uris;

    {
        std::shared_lock lk(pending_mu_);
        for (auto& [uri, queue] : pending_messages_) {
            if (queue.empty()) continue;
            const auto age = now - queue.front().queued_at;
            if (age > PENDING_TTL) {
                stale_uris.push_back(uri);
            }
        }
    }

    if (!stale_uris.empty()) {
        std::unique_lock lk(pending_mu_);
        for (auto& uri : stale_uris) {
            auto it = pending_messages_.find(uri);
            if (it == pending_messages_.end()) continue;

            auto& queue = it->second;
            auto remove_from = std::remove_if(queue.begin(), queue.end(),
                [now](const PendingMessage& msg) {
                    return (now - msg.queued_at) > PENDING_TTL;
                });

            size_t removed = std::distance(remove_from, queue.end());
            queue.erase(remove_from, queue.end());

            if (removed > 0)
                LOG_WARN("Dropped {} stale pending messages for URI: {}", removed, uri);

            if (queue.empty())
                pending_messages_.erase(it);
        }
    }
}

} // namespace gn