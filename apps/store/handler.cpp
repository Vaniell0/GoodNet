/// @file apps/store/handler.cpp
/// @brief Wire protocol handler для Store.

#include "handler.hpp"
#include "core/data/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gn::store {

using namespace gn::msg;

// ── Helpers ──────────────────────────────────────────────────────────────────

static uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

/// Извлечь variable-length данные после фиксированного заголовка.
static std::pair<std::string_view, std::span<const uint8_t>>
extract_kv(const uint8_t* payload, size_t total_len,
           size_t header_sz, uint16_t key_len, uint16_t value_len) {
    if (total_len < header_sz + key_len + value_len)
        return {{}, {}};
    const char* key_ptr = reinterpret_cast<const char*>(payload + header_sz);
    const uint8_t* val_ptr = payload + header_sz + key_len;
    return {
        std::string_view(key_ptr, key_len),
        std::span<const uint8_t>(val_ptr, value_len)
    };
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

StoreHandler::StoreHandler(gn::Core& core, IStore& backend)
    : core_(core), backend_(backend) {}

StoreHandler::~StoreHandler() {
    stop();
}

void StoreHandler::start() {
    auto sub = [&](uint32_t msg_type, auto method) {
        sub_ids_.push_back(core_.subscribe(msg_type, "store",
            [this, method](std::string_view name, std::shared_ptr<header_t> hdr,
                           const endpoint_t* ep, PacketData data) {
                return (this->*method)(name, std::move(hdr), ep, std::move(data));
            }));
    };

    sub(MSG_TYPE_SYS_STORE_PUT,       &StoreHandler::on_put);
    sub(MSG_TYPE_SYS_STORE_GET,       &StoreHandler::on_get);
    sub(MSG_TYPE_SYS_STORE_DELETE,    &StoreHandler::on_delete);
    sub(MSG_TYPE_SYS_STORE_SUBSCRIBE, &StoreHandler::on_subscribe);
    sub(MSG_TYPE_SYS_STORE_SYNC,      &StoreHandler::on_sync);
}

void StoreHandler::stop() {
    for (auto id : sub_ids_)
        core_.unsubscribe(id);
    sub_ids_.clear();
}

void StoreHandler::on_disconnect(conn_id_t id) {
    std::lock_guard lock(sub_mu_);
    std::erase_if(subscriptions_, [id](const Sub& s) { return s.conn_id == id; });
}

// ── PUT ──────────────────────────────────────────────────────────────────────

propagation_t StoreHandler::on_put(
        std::string_view, std::shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data) {

    const auto& raw = *data;
    if (raw.size() < sizeof(StorePutPayload))
        return PROPAGATION_REJECT;

    StorePutPayload put;
    std::memcpy(&put, raw.data(), sizeof(put));

    if (put.key_len > STORE_KEY_MAX_LEN || put.value_len > STORE_VALUE_MAX_LEN)
        return PROPAGATION_REJECT;

    auto [key, value] = extract_kv(raw.data(), raw.size(),
                                   sizeof(StorePutPayload), put.key_len, put.value_len);
    if (key.empty() && put.key_len > 0)
        return PROPAGATION_REJECT;

    bool ok = backend_.put(key, value, put.ttl_s, put.flags);

    // Отправить результат
    uint8_t status = ok ? 0 : 2;
    auto resp = serialize_result(put.request_id, {}, status);
    core_.send(ep->peer_id, MSG_TYPE_SYS_STORE_RESULT, std::span{resp});

    // Нотифицировать подписчиков
    if (ok) {
        Entry e;
        e.key = std::string(key);
        e.value.assign(value.begin(), value.end());
        e.timestamp_us = now_us();
        e.ttl_s = put.ttl_s;
        e.flags = put.flags;
        notify_subscribers(e, 0 /* created/updated */);
    }

    return PROPAGATION_CONSUMED;
}

// ── GET ──────────────────────────────────────────────────────────────────────

propagation_t StoreHandler::on_get(
        std::string_view, std::shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data) {

    const auto& raw = *data;
    if (raw.size() < sizeof(StoreGetPayload))
        return PROPAGATION_REJECT;

    StoreGetPayload get;
    std::memcpy(&get, raw.data(), sizeof(get));

    if (get.key_len > STORE_KEY_MAX_LEN)
        return PROPAGATION_REJECT;

    std::string_view key;
    if (get.key_len > 0 && raw.size() >= sizeof(StoreGetPayload) + get.key_len)
        key = std::string_view(
            reinterpret_cast<const char*>(raw.data() + sizeof(StoreGetPayload)),
            get.key_len);

    uint32_t max_results = get.max_results ? get.max_results : 32;
    std::vector<Entry> entries;

    switch (get.query_type) {
        case 0: { // exact
            auto e = backend_.get(key);
            if (e) entries.push_back(std::move(*e));
            break;
        }
        case 1: // prefix
        case 2: // list namespace (= prefix with /)
            entries = backend_.get_prefix(key, max_results);
            break;
        default:
            break;
    }

    uint8_t status = entries.empty() ? 1 : 0;  // not_found : ok
    auto resp = serialize_result(get.request_id, entries, status);
    core_.send(ep->peer_id, MSG_TYPE_SYS_STORE_RESULT, std::span{resp});

    return PROPAGATION_CONSUMED;
}

// ── DELETE ───────────────────────────────────────────────────────────────────

propagation_t StoreHandler::on_delete(
        std::string_view, std::shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data) {

    const auto& raw = *data;
    if (raw.size() < sizeof(StoreDeletePayload))
        return PROPAGATION_REJECT;

    StoreDeletePayload del;
    std::memcpy(&del, raw.data(), sizeof(del));

    if (del.key_len > STORE_KEY_MAX_LEN)
        return PROPAGATION_REJECT;

    std::string_view key;
    if (del.key_len > 0 && raw.size() >= sizeof(StoreDeletePayload) + del.key_len)
        key = std::string_view(
            reinterpret_cast<const char*>(raw.data() + sizeof(StoreDeletePayload)),
            del.key_len);

    bool existed = backend_.del(key);

    uint8_t status = existed ? 0 : 1;
    auto resp = serialize_result(del.request_id, {}, status);
    core_.send(ep->peer_id, MSG_TYPE_SYS_STORE_RESULT, std::span{resp});

    // Нотифицировать подписчиков
    if (existed) {
        Entry e;
        e.key = std::string(key);
        e.timestamp_us = now_us();
        notify_subscribers(e, 2 /* deleted */);
    }

    return PROPAGATION_CONSUMED;
}

// ── SUBSCRIBE ────────────────────────────────────────────────────────────────

propagation_t StoreHandler::on_subscribe(
        std::string_view, std::shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data) {

    const auto& raw = *data;
    if (raw.size() < sizeof(StoreSubscribePayload))
        return PROPAGATION_REJECT;

    StoreSubscribePayload sub;
    std::memcpy(&sub, raw.data(), sizeof(sub));

    std::string_view key;
    if (sub.key_len > 0 && raw.size() >= sizeof(StoreSubscribePayload) + sub.key_len)
        key = std::string_view(
            reinterpret_cast<const char*>(raw.data() + sizeof(StoreSubscribePayload)),
            sub.key_len);

    std::lock_guard lock(sub_mu_);

    if (sub.action == 0) {
        // Подписка
        subscriptions_.push_back({ep->peer_id, std::string(key), sub.query_type});
    } else {
        // Отписка
        std::erase_if(subscriptions_, [&](const Sub& s) {
            return s.conn_id == ep->peer_id && s.key == key;
        });
    }

    // Подтверждение
    auto resp = serialize_result(sub.request_id, {}, 0);
    core_.send(ep->peer_id, MSG_TYPE_SYS_STORE_RESULT, std::span{resp});

    return PROPAGATION_CONSUMED;
}

// ── SYNC ─────────────────────────────────────────────────────────────────────

propagation_t StoreHandler::on_sync(
        std::string_view, std::shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data) {

    const auto& raw = *data;
    if (raw.size() < sizeof(StoreSyncPayload))
        return PROPAGATION_REJECT;

    StoreSyncPayload sync;
    std::memcpy(&sync, raw.data(), sizeof(sync));

    if (sync.sync_type == 0) {
        // Запрос full/delta sync — отправить наши записи
        auto entries = backend_.get_since(sync.since_timestamp, 256);

        // Сериализовать ответ
        StoreSyncPayload resp_hdr{};
        resp_hdr.request_id = sync.request_id;
        resp_hdr.sync_type = 1;  // full_response
        resp_hdr.entry_count = static_cast<uint8_t>(
            std::min(entries.size(), static_cast<size_t>(255)));

        std::vector<uint8_t> buf(sizeof(resp_hdr));
        std::memcpy(buf.data(), &resp_hdr, sizeof(resp_hdr));

        for (size_t i = 0; i < resp_hdr.entry_count; ++i) {
            auto entry_buf = serialize_entry(entries[i]);
            buf.insert(buf.end(), entry_buf.begin(), entry_buf.end());
        }

        core_.send(ep->peer_id, MSG_TYPE_SYS_STORE_SYNC, std::span{buf});

    } else if (sync.sync_type == 1 || sync.sync_type == 2) {
        // Получили sync response — импортировать записи
        const uint8_t* ptr = raw.data() + sizeof(StoreSyncPayload);
        size_t remaining = raw.size() - sizeof(StoreSyncPayload);

        for (uint8_t i = 0; i < sync.entry_count && remaining >= sizeof(StoreEntry); ++i) {
            StoreEntry se;
            std::memcpy(&se, ptr, sizeof(se));
            ptr += sizeof(se);
            remaining -= sizeof(se);

            if (remaining < se.key_len + se.value_len) break;

            std::string_view key(reinterpret_cast<const char*>(ptr), se.key_len);
            ptr += se.key_len;
            remaining -= se.key_len;

            std::span<const uint8_t> value(ptr, se.value_len);
            ptr += se.value_len;
            remaining -= se.value_len;

            // Только если наша запись старше
            auto existing = backend_.get(key);
            if (!existing || existing->timestamp_us < se.timestamp_us) {
                backend_.put(key, value, se.ttl_s, se.flags);
            }
        }
    }

    return PROPAGATION_CONSUMED;
}

// ── Notifications ────────────────────────────────────────────────────────────

void StoreHandler::notify_subscribers(const Entry& entry, uint8_t event) {
    std::lock_guard lock(sub_mu_);

    for (const auto& sub : subscriptions_) {
        bool match = false;
        if (sub.query_type == 0) {
            // Exact match
            match = (sub.key == entry.key);
        } else {
            // Prefix match
            match = entry.key.starts_with(sub.key);
        }

        if (match) {
            auto buf = serialize_notify(entry, event);
            core_.send(sub.conn_id, MSG_TYPE_SYS_STORE_NOTIFY, std::span{buf});
        }
    }
}

// ── Serialization ────────────────────────────────────────────────────────────

std::vector<uint8_t> StoreHandler::serialize_entry(const Entry& e) {
    StoreEntry se{};
    se.key_len = static_cast<uint16_t>(e.key.size());
    se.value_len = static_cast<uint16_t>(e.value.size());
    se.timestamp_us = e.timestamp_us;
    se.ttl_s = e.ttl_s;
    se.flags = e.flags;

    std::vector<uint8_t> buf(sizeof(se) + se.key_len + se.value_len);
    std::memcpy(buf.data(), &se, sizeof(se));
    std::memcpy(buf.data() + sizeof(se), e.key.data(), se.key_len);
    if (!e.value.empty())
        std::memcpy(buf.data() + sizeof(se) + se.key_len, e.value.data(), se.value_len);
    return buf;
}

std::vector<uint8_t> StoreHandler::serialize_result(
        uint64_t request_id, const std::vector<Entry>& entries, uint8_t status) {

    StoreResultPayload hdr{};
    hdr.request_id = request_id;
    hdr.entry_count = static_cast<uint8_t>(
        std::min(entries.size(), static_cast<size_t>(255)));
    hdr.status = status;
    hdr.total_count = static_cast<uint32_t>(entries.size());

    std::vector<uint8_t> buf(sizeof(hdr));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    for (size_t i = 0; i < hdr.entry_count; ++i) {
        auto entry_buf = serialize_entry(entries[i]);
        buf.insert(buf.end(), entry_buf.begin(), entry_buf.end());
    }

    return buf;
}

std::vector<uint8_t> StoreHandler::serialize_notify(const Entry& entry, uint8_t event) {
    StoreNotifyPayload hdr{};
    hdr.event = event;
    hdr.timestamp_us = entry.timestamp_us ? entry.timestamp_us : now_us();

    auto entry_buf = serialize_entry(entry);

    std::vector<uint8_t> buf(sizeof(hdr) + entry_buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), entry_buf.data(), entry_buf.size());
    return buf;
}

} // namespace gn::store
