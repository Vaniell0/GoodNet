/// @file plugins/connectors/ice/ice.cpp
/// @brief ICE/DTLS connector plugin (libnice, RFC 8445).
///
/// Scheme: "ice"
/// Connect URI: "ice://<peer_pubkey_hex_64>"
///
/// Signaling model:
///   A ──[TCP AUTH]──► B         handled by TCP connector + core
///   A ──[ICE_OFFER]──► B        MSG_TYPE_ICE_SIGNAL, kind=OFFER
///   B ──[ICE_ANSWER]──► A       MSG_TYPE_ICE_SIGNAL, kind=ANSWER
///   A/B ── UDP/DTLS ──           direct ICE data path
///
/// Threading: all NiceAgent / GObject calls happen on a private GLib thread.
///            Public methods post lambdas via g_main_context_invoke (thread-safe).

#include <logger.hpp>

#include <cpp/connector.hpp>
#include <connector.h>
#include <handler.h>
#include <types.h>

#include <nice/nice.h>
#include <glib.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>

namespace gn {

// ── Wire types ────────────────────────────────────────────────────────────────

enum class IceSignalKind : uint8_t { OFFER = 0, ANSWER = 1 };

#pragma pack(push, 1)
struct IceSignalHdr {
    uint8_t  kind;
    uint8_t  _pad[3];
    uint32_t sdp_len;
    // sdp_len bytes of UTF-8 SDP follow immediately
};
#pragma pack(pop)

// ── Per-peer session state ────────────────────────────────────────────────────

enum class IceSessState : uint8_t {
    Gathering, Signaling, Connecting, Connected, Failed
};

struct IceSess {
    NiceAgent*  agent     = nullptr;
    guint       stream_id = 0;
    std::string peer_hex;
    conn_id_t   conn_id   = CONN_ID_INVALID;
    IceSignalKind our_kind{};
    std::atomic<IceSessState> state{IceSessState::Gathering};
    class IceConnector* owner = nullptr;
};

// ── Connector ─────────────────────────────────────────────────────────────────

class IceConnector : public IConnector {
public:
    std::string get_scheme() const override { return "ice"; }
    std::string get_name()   const override { return "GoodNet ICE/DTLS (libnice)"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void on_init() override {
        gctx_  = g_main_context_new();
        gloop_ = g_main_loop_new(gctx_, FALSE);
        glib_thread_ = std::thread([this] {
            g_main_context_push_thread_default(gctx_);
            g_main_loop_run(gloop_);
            g_main_context_pop_thread_default(gctx_);
        });

        sig_handler_.name                = "ice_signal_handler";
        sig_handler_.user_data           = this;
        sig_handler_.handle_message      = s_on_signal;
        sig_handler_.on_message_result   = nullptr;
        sig_handler_.handle_conn_state   = nullptr;
        sig_handler_.shutdown            = nullptr;
        sig_handler_.supported_types     = &kSigType;
        sig_handler_.num_supported_types = 1;
        register_extra_handler(&sig_handler_);

        LOG_INFO("[ICE] connector ready");
    }

    void on_shutdown() override {
        invoke([this] {
            std::lock_guard lk(mu_);
            for (auto& [k, s] : sessions_) teardown(*s);
            sessions_.clear();
        });
        g_main_loop_quit(gloop_);
        if (glib_thread_.joinable()) glib_thread_.join();
        g_main_loop_unref(gloop_);
        g_main_context_unref(gctx_);
    }

    // ── do_* ─────────────────────────────────────────────────────────────────

    int do_connect(const char* uri) override {
        std::string target(uri);
        if (auto p = target.find("://"); p != std::string::npos)
            target = target.substr(p + 3);
        if (target.size() != 64) {
            LOG_ERROR("[ICE] do_connect: need 64-char pubkey hex, got '{}'", target);
            return -1;
        }
        invoke([this, peer = std::move(target)] {
            create_session(peer, IceSignalKind::OFFER);
        });
        return 0;
    }

    int do_listen(const char*, uint16_t) override { return 0; }

    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        auto buf = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
        invoke([this, id, buf] {
            auto s = by_conn(id);
            if (!s || s->state.load() != IceSessState::Connected) return;
            nice_agent_send(s->agent, s->stream_id, 1,
                             static_cast<guint>(buf->size()),
                             reinterpret_cast<const gchar*>(buf->data()));
        });
        return 0;
    }

    void do_close(conn_id_t id, bool /*hard*/) override {
        invoke([this, id] {
            std::lock_guard lk(mu_);
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                if (it->second->conn_id != id) continue;
                teardown(*it->second);
                notify_disconnect(id, 0);
                sessions_.erase(it);
                return;
            }
        });
    }

    // ── Incoming ICE signal (from core bus) ───────────────────────────────────

    void on_ice_signal(const endpoint_t* ep, std::span<const uint8_t> pl) {
        if (pl.size() < sizeof(IceSignalHdr)) return;
        const auto* h = reinterpret_cast<const IceSignalHdr*>(pl.data());
        if (pl.size() < sizeof(IceSignalHdr) + h->sdp_len) return;

        std::string sdp(reinterpret_cast<const char*>(pl.data() + sizeof(IceSignalHdr)),
                         h->sdp_len);
        const auto kind     = static_cast<IceSignalKind>(h->kind);
        std::string peer    = hex32(ep->pubkey);
        const conn_id_t sig = ep->peer_id;

        invoke([this, peer, sdp, kind, sig] {
            if (kind == IceSignalKind::OFFER) {
                if (auto s = create_session(peer, IceSignalKind::ANSWER))
                    apply_remote(s, sdp, sig);
            } else {
                if (auto s = by_peer(peer))
                    apply_remote(s, sdp, sig);
            }
        });
    }

private:
    // ── Session management (GLib thread) ──────────────────────────────────────

    std::shared_ptr<IceSess> create_session(const std::string& peer,
                                             IceSignalKind kind) {
        {
            std::lock_guard lk(mu_);
            if (sessions_.count(peer)) return nullptr;
        }

        auto s     = std::make_shared<IceSess>();
        s->peer_hex = peer;
        s->our_kind = kind;
        s->owner    = this;

        s->agent = nice_agent_new_full(gctx_, NICE_COMPATIBILITY_RFC5245,
                                        NICE_AGENT_OPTION_REGULAR_NOMINATION);
        if (!s->agent) return nullptr;

        g_object_set(s->agent,
                     "stun-server",      "stun.l.google.com",
                     "stun-server-port", 19302,
                     "controlling-mode", (kind == IceSignalKind::OFFER),
                     nullptr);

        s->stream_id = nice_agent_add_stream(s->agent, 1);
        nice_agent_set_stream_name(s->agent, s->stream_id, "application");

        g_signal_connect(s->agent, "candidate-gathering-done",
                         G_CALLBACK(s_gather_done),   s.get());
        g_signal_connect(s->agent, "component-state-changed",
                         G_CALLBACK(s_state_changed), s.get());
        nice_agent_attach_recv(s->agent, s->stream_id, 1, gctx_,
                                s_recv, s.get());

        {
            std::lock_guard lk(mu_);
            sessions_[peer] = s;
        }

        nice_agent_gather_candidates(s->agent, s->stream_id);
        return s;
    }

    void apply_remote(std::shared_ptr<IceSess> s,
                       const std::string& sdp, conn_id_t sig) {
        if (nice_agent_parse_remote_sdp(s->agent, sdp.c_str()) < 0) {
            s->state.store(IceSessState::Failed);
            return;
        }
        s->state.store(IceSessState::Connecting);
        if (s->our_kind == IceSignalKind::ANSWER)
            send_sdp(s, sig);
    }

    void send_sdp(std::shared_ptr<IceSess> s, conn_id_t sig_conn) {
        gchar* raw = nice_agent_generate_local_sdp(s->agent);
        if (!raw) return;
        const std::string sdp(raw); g_free(raw);

        const uint32_t sdp_len = static_cast<uint32_t>(sdp.size());
        std::vector<uint8_t> pkt(sizeof(IceSignalHdr) + sdp_len);
        auto* hdr    = reinterpret_cast<IceSignalHdr*>(pkt.data());
        hdr->kind    = static_cast<uint8_t>(s->our_kind);
        hdr->sdp_len = sdp_len;
        std::memcpy(pkt.data() + sizeof(IceSignalHdr), sdp.data(), sdp_len);

        const std::span<const uint8_t> sp(pkt);
        if (sig_conn != CONN_ID_INVALID)
            api_->send_response(api_->ctx, sig_conn, MSG_TYPE_ICE_SIGNAL,
                                 sp.data(), sp.size());
        else
            api_->send(api_->ctx,
                        ("ice://" + s->peer_hex).c_str(),
                        MSG_TYPE_ICE_SIGNAL, sp.data(), sp.size());

        s->state.store(IceSessState::Signaling);
    }

    void teardown(IceSess& s) noexcept {
        if (!s.agent) return;
        nice_agent_remove_stream(s.agent, s.stream_id);
        g_object_unref(s.agent);
        s.agent = nullptr;
    }

    void ice_connected(IceSess* s) {
        endpoint_t ep{};
        std::strncpy(ep.address, s->peer_hex.c_str(), sizeof(ep.address) - 1);
        const conn_id_t cid = notify_connect(&ep);
        if (cid == CONN_ID_INVALID) return;
        s->conn_id = cid;
        LOG_INFO("[ICE] connected conn_id={} peer={:.8}...", cid, s->peer_hex);
    }

    // ── GObject callbacks ─────────────────────────────────────────────────────

    static void s_gather_done(NiceAgent*, guint, gpointer ud) {
        auto* s = static_cast<IceSess*>(ud);
        if (s->our_kind != IceSignalKind::OFFER) return;
        s->owner->invoke([s] {
            conn_id_t sig = s->owner->find_peer_conn(s->peer_hex.c_str());
            if (sig != CONN_ID_INVALID)
                if (auto sp = s->owner->by_peer(s->peer_hex))
                    s->owner->send_sdp(sp, sig);
        });
    }

    static void s_state_changed(NiceAgent*, guint, guint,
                                  guint state, gpointer ud) {
        auto* s = static_cast<IceSess*>(ud);
        if (state == NICE_COMPONENT_STATE_READY) {
            if (s->state.exchange(IceSessState::Connected) != IceSessState::Connected)
                s->owner->invoke([s] { s->owner->ice_connected(s); });
        } else if (state == NICE_COMPONENT_STATE_FAILED) {
            s->state.store(IceSessState::Failed);
            if (s->conn_id != CONN_ID_INVALID)
                s->owner->notify_disconnect(s->conn_id, EIO);
        }
    }

    static void s_recv(NiceAgent*, guint, guint, guint len,
                        gchar* buf, gpointer ud) {
        auto* s = static_cast<IceSess*>(ud);
        if (s->conn_id == CONN_ID_INVALID) return;
        s->owner->notify_data(s->conn_id,
            {reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len)});
    }

    static void s_on_signal(void* ud, const header_t*,
                              const endpoint_t* ep,
                              const void* pl, size_t sz) {
        static_cast<IceConnector*>(ud)->on_ice_signal(
            ep, {static_cast<const uint8_t*>(pl), sz});
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    template<typename F>
    void invoke(F&& fn) {
        auto* p = new std::function<void()>(std::forward<F>(fn));
        g_main_context_invoke_full(gctx_, G_PRIORITY_DEFAULT,
            [](gpointer d) -> gboolean {
                auto* f = static_cast<std::function<void()>*>(d);
                (*f)(); delete f; return G_SOURCE_REMOVE;
            }, p, nullptr);
    }

    std::shared_ptr<IceSess> by_peer(const std::string& h) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(h);
        return it != sessions_.end() ? it->second : nullptr;
    }
    std::shared_ptr<IceSess> by_conn(conn_id_t id) {
        std::lock_guard lk(mu_);
        for (auto& [k, v] : sessions_) if (v->conn_id == id) return v;
        return nullptr;
    }

    static std::string hex32(const uint8_t* pk) {
        std::string out(64, '\0');
        for (int i = 0; i < 32; ++i)
            std::snprintf(&out[i * 2], 3, "%02x", pk[i]);
        return out;
    }

    // ── Members ───────────────────────────────────────────────────────────────

    GMainContext* gctx_  = nullptr;
    GMainLoop*    gloop_ = nullptr;
    std::thread   glib_thread_;

    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<IceSess>> sessions_;

    handler_t sig_handler_{};
    static constexpr uint32_t kSigType = MSG_TYPE_ICE_SIGNAL;
};

} // namespace gn

CONNECTOR_PLUGIN(gn::IceConnector)