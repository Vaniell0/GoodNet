#pragma once
/// @file tests/test_helpers.hpp
/// Общие утилиты для тестов: tmp_dir, mock connectors, SSH key helpers.

#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "types/connection.hpp"
#include "../sdk/connector.h"

namespace fs = std::filesystem;

// ─── Временная директория ────────────────────────────────────────────────────

inline fs::path tmp_dir(const std::string& suffix = "") {
    auto p = fs::temp_directory_path() / ("gn_test_" + suffix + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

// ─── No-op mock connector ────────────────────────────────────────────────────

inline connector_ops_t make_mock_connector_ops() {
    connector_ops_t ops{};
    ops.connect    = [](void*, const char*) -> int { return -1; };
    ops.listen     = [](void*, const char*, uint16_t) -> int { return 0; };
    ops.send_to    = [](void*, conn_id_t, const void*, size_t) -> int { return 0; };
    ops.close      = [](void*, conn_id_t) {};
    ops.get_scheme = [](void*, char* b, size_t s) { strncpy(b, "mock", s); };
    ops.get_name   = [](void*, char* b, size_t s) { strncpy(b, "MockConnector", s); };
    ops.shutdown   = [](void*) {};
    ops.connector_ctx = nullptr;
    return ops;
}

// ─── Capturing mock connector ────────────────────────────────────────────────

struct CapturedFrame {
    conn_id_t id;
    std::vector<uint8_t> data;
};

struct CapturingSink {
    std::mutex mu;
    std::vector<CapturedFrame> frames;

    void clear() { std::lock_guard lk(mu); frames.clear(); }

    std::vector<CapturedFrame> get_noise_frames() {
        std::lock_guard lk(mu);
        std::vector<CapturedFrame> result;
        for (auto& f : frames) {
            if (f.data.size() < sizeof(header_t)) continue;
            auto* hdr = reinterpret_cast<const header_t*>(f.data.data());
            if (hdr->payload_type == MSG_TYPE_NOISE_INIT ||
                hdr->payload_type == MSG_TYPE_NOISE_RESP ||
                hdr->payload_type == MSG_TYPE_NOISE_FIN)
                result.push_back(f);
        }
        return result;
    }

    /// Извлечь первый кадр заданного типа, удалив его из списка.
    std::vector<uint8_t> extract(uint16_t msg_type) {
        std::lock_guard lk(mu);
        for (auto it = frames.begin(); it != frames.end(); ++it) {
            if (it->data.size() < sizeof(header_t)) continue;
            auto* hdr = reinterpret_cast<const header_t*>(it->data.data());
            if (hdr->payload_type == msg_type) {
                auto d = std::move(it->data);
                frames.erase(it);
                return d;
            }
        }
        return {};
    }

    /// Подсчёт кадров заданного типа.
    size_t count_frames(uint16_t msg_type) {
        std::lock_guard lk(mu);
        size_t n = 0;
        for (auto& f : frames) {
            if (f.data.size() < sizeof(header_t)) continue;
            auto* hdr = reinterpret_cast<const header_t*>(f.data.data());
            if (hdr->payload_type == msg_type) ++n;
        }
        return n;
    }
};

inline CapturingSink* g_cap_sink = nullptr;

inline connector_ops_t make_capturing_connector(CapturingSink* sink) {
    g_cap_sink = sink;
    connector_ops_t ops{};
    ops.connect    = [](void*, const char*) -> int { return -1; };
    ops.listen     = [](void*, const char*, uint16_t) -> int { return 0; };
    ops.send_to    = [](void*, conn_id_t id, const void* data, size_t sz) -> int {
        if (g_cap_sink) {
            std::lock_guard lk(g_cap_sink->mu);
            CapturedFrame cf;
            cf.id = id;
            cf.data.assign(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + sz);
            g_cap_sink->frames.push_back(std::move(cf));
        }
        return 0;
    };
    ops.close      = [](void*, conn_id_t) {};
    ops.get_scheme = [](void*, char* b, size_t s) { strncpy(b, "mock", s); };
    ops.get_name   = [](void*, char* b, size_t s) { strncpy(b, "MockCap", s); };
    ops.shutdown   = [](void*) {};
    ops.connector_ctx = nullptr;
    return ops;
}

// ─── OpenSSH Ed25519 key helpers ─────────────────────────────────────────────

inline std::string make_openssh_pem(const uint8_t pub[32], const uint8_t sec[64]) {
    auto write_u32be = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x >> 24); v.push_back((x >> 16) & 0xFF);
        v.push_back((x >>  8) & 0xFF); v.push_back(x & 0xFF);
    };
    auto write_str = [&](std::vector<uint8_t>& v, std::string_view s) {
        write_u32be(v, (uint32_t)s.size());
        v.insert(v.end(), s.begin(), s.end());
    };
    auto write_blob = [&](std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
        write_u32be(v, (uint32_t)n);
        v.insert(v.end(), d, d + n);
    };

    std::vector<uint8_t> priv;
    uint32_t check = 0xDEADBEEF;
    write_u32be(priv, check);
    write_u32be(priv, check);
    write_str(priv, "ssh-ed25519");
    write_blob(priv, pub, 32);
    write_blob(priv, sec, 64);
    write_str(priv, "test-comment");
    for (uint8_t i = 1; priv.size() % 8 != 0; ++i) priv.push_back(i);

    std::vector<uint8_t> pub_blob;
    write_str(pub_blob, "ssh-ed25519");
    write_blob(pub_blob, pub, 32);

    std::vector<uint8_t> blob;
    const uint8_t magic[] = {'o','p','e','n','s','s','h','-','k','e','y','-','v','1','\0'};
    blob.insert(blob.end(), magic, magic + sizeof(magic));
    write_str(blob, "none");
    write_str(blob, "none");
    write_str(blob, "");
    write_u32be(blob, 1);
    write_blob(blob, pub_blob.data(), pub_blob.size());
    write_blob(blob, priv.data(), priv.size());

    std::string b64;
    b64.reserve(blob.size() * 4 / 3 + 4);
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i + 2 < blob.size()) {
        uint32_t v = (uint32_t(blob[i])<<16)|(uint32_t(blob[i+1])<<8)|blob[i+2];
        b64 += T[(v>>18)&63]; b64 += T[(v>>12)&63];
        b64 += T[(v>> 6)&63]; b64 += T[v&63];
        i += 3;
    }
    if (i < blob.size()) {
        uint32_t v = uint32_t(blob[i]) << 16;
        if (i + 1 < blob.size()) v |= uint32_t(blob[i+1]) << 8;
        b64 += T[(v>>18)&63]; b64 += T[(v>>12)&63];
        b64 += (i + 1 < blob.size()) ? T[(v>>6)&63] : '=';
        b64 += '=';
    }

    std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    for (size_t j = 0; j < b64.size(); j += 70)
        pem += b64.substr(j, 70) + "\n";
    pem += "-----END OPENSSH PRIVATE KEY-----\n";
    return pem;
}

inline fs::path write_openssh_pem(const fs::path& dir,
                                   const uint8_t pub[32], const uint8_t sec[64]) {
    fs::path p = dir / "id_ed25519";
    std::ofstream f(p);
    f << make_openssh_pem(pub, sec);
    return p;
}
