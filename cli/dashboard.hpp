#pragma once
/// @file cli/dashboard.hpp
/// @brief Formatting helpers, live dashboard, and summary table for CLI output.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "signals.hpp"

namespace cli {

// ─── Throughput sample ring ──────────────────────────────────────────────────
/// Keeps the last N throughput samples (Gbps) for percentile computation.

static constexpr size_t SAMPLE_RING = 1200;  ///< 10 min at 0.5s interval.

struct Samples {
    std::array<double, SAMPLE_RING> data{};
    size_t head  = 0;
    size_t count = 0;

    void push(double v) {
        data[head] = v;
        head = (head + 1) % SAMPLE_RING;
        if (count < SAMPLE_RING) ++count;
    }

    std::vector<double> sorted() const {
        std::vector<double> v(data.begin(), data.begin() + count);
        std::sort(v.begin(), v.end());
        return v;
    }

    double percentile(double p) const {
        auto s = sorted();
        if (s.empty()) return 0.0;
        const size_t idx = static_cast<size_t>(p / 100.0 * (s.size() - 1));
        return s[std::min(idx, s.size() - 1)];
    }

    double mean() const {
        if (count == 0) return 0.0;
        double sum = 0;
        for (size_t i = 0; i < count; ++i) sum += data[i];
        return sum / count;
    }

    double peak() const {
        if (count == 0) return 0.0;
        return *std::max_element(data.begin(), data.begin() + count);
    }
};

// ─── Formatting helpers ──────────────────────────────────────────────────────

inline std::string fmt_bytes(double bytes) {
    char b[32];
    if (bytes < 1e3)  return std::to_string(static_cast<int>(bytes)) + " B";
    if (bytes < 1e6)  { std::snprintf(b, sizeof(b), "%.1f KB", bytes / 1e3); return b; }
    if (bytes < 1e9)  { std::snprintf(b, sizeof(b), "%.1f MB", bytes / 1e6); return b; }
                        { std::snprintf(b, sizeof(b), "%.2f GB", bytes / 1e9); return b; }
}

inline std::string fmt_duration(double sec) {
    const int h = static_cast<int>(sec) / 3600;
    const int m = (static_cast<int>(sec) % 3600) / 60;
    const int s = static_cast<int>(sec) % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    else       std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    return buf;
}

inline std::string fmt_num(uint64_t n) {
    std::string s = std::to_string(n);
    int insert_at = static_cast<int>(s.length()) - 3;
    while (insert_at > 0) {
        s.insert(insert_at, ",");
        insert_at -= 3;
    }
    return s;
}

inline std::string bar(double fraction, int width = 20) {
    const int filled = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * width);
    std::string b;
    b.reserve(width * 4);
    for (int i = 0; i < filled; ++i)    b += "\u2588";  // █
    for (int i = filled; i < width; ++i) b += "\u2591";  // ░
    return b;
}

/// Counts visible characters in a UTF-8 string (skipping continuation bytes).
inline int utf8_len(const std::string& str) {
    int len = 0;
    for (size_t i = 0; i < str.length(); ++i) {
        if ((str[i] & 0xc0) != 0x80) len++;
    }
    return len;
}

// ─── Box-drawing constants ───────────────────────────────────────────────────

static constexpr const char* BOX_H   = "\u2550";  // ═
static constexpr const char* BOX_V   = "\u2551";  // ║
static constexpr const char* BOX_TL  = "\u2554";  // ╔
static constexpr const char* BOX_TR  = "\u2557";  // ╗
static constexpr const char* BOX_BL  = "\u255A";  // ╚
static constexpr const char* BOX_BR  = "\u255D";  // ╝
static constexpr const char* BOX_ML  = "\u2560";  // ╠
static constexpr const char* BOX_MR  = "\u2563";  // ╣
static constexpr const char* BOX_SEP = "\u2502";  // │

/// Print a horizontal box line:  ╔════...════╗  or  ╠════...════╣  or  ╚════...════╝
inline void box_line(const char* left, const char* right, int width = 58) {
    std::printf("%s", left);
    for (int i = 0; i < width; ++i) std::printf("%s", BOX_H);
    std::printf("%s\n", right);
}

// ─── Table row helper ────────────────────────────────────────────────────────

inline void print_row(const char* label, const std::string& value,
                      const char* color = "") {
    const int total_width = 58;

    std::string prefix = "  ";
    prefix += label;
    prefix += " : ";

    int prefix_vis_len = utf8_len(prefix);
    int value_vis_len  = utf8_len(value);

    std::printf("%s%s%s%s\033[0m", BOX_V, prefix.c_str(), color, value.c_str());

    int padding = total_width - prefix_vis_len - value_vis_len;
    for (int i = 0; i < padding; ++i) std::printf(" ");
    std::printf("%s\n", BOX_V);
}

// ─── Live client dashboard ───────────────────────────────────────────────────

inline void print_live(double gbps, double gbps_peak,
                       double pkt_s, double total_sec,
                       uint64_t total_bytes, uint64_t total_pkts,
                       double backlog_mb,
                       const gn::StatsSnapshot& st) {
    std::printf(
        "\033[2K\r"
        "[%s] "
        "\033[1;36m%.2f Gbps\033[0m (peak \033[33m%.2f\033[0m) | "
        "\033[1;32m%7.0f pkt/s\033[0m | "
        "Sent \033[1m%s\033[0m (%llu pkts) | "
        "Backlog \033[%sm%.1f MB\033[0m | "
        "RX %s | TX %s | "
        "Auth \u2713%llu \u2717%llu | "
        "Drops %llu",
        fmt_duration(total_sec).c_str(),
        gbps,
        gbps_peak,
        pkt_s,
        fmt_bytes(static_cast<double>(total_bytes)).c_str(),
        (unsigned long long)total_pkts,
        (backlog_mb > 64) ? "1;31" : "0",
        backlog_mb,
        fmt_bytes(static_cast<double>(st.rx_bytes)).c_str(),
        fmt_bytes(static_cast<double>(st.tx_bytes)).c_str(),
        (unsigned long long)st.auth_ok,
        (unsigned long long)st.auth_fail,
        (unsigned long long)st.backpressure);
    std::fflush(stdout);
}

// ─── Live server dashboard ───────────────────────────────────────────────────

inline void print_server_line(double total_sec, unsigned connections,
                              double rx_gbps, double tx_gbps, double pkt_s,
                              const gn::StatsSnapshot& st) {
    std::printf(
        "\033[2K\r"
        "[Server %s] "
        "conns=\033[1;32m%u\033[0m  "
        "RX \033[1;36m%.2f\033[0m Gbps  "
        "TX \033[1;33m%.2f\033[0m Gbps  "
        "%.0f pkt/s  "
        "auth\u2713\033[32m%llu\033[0m \u2717\033[31m%llu\033[0m  "
        "dec_fail \033[%s%llu\033[0m  "
        "drops=%llu",
        fmt_duration(total_sec).c_str(),
        connections,
        rx_gbps, tx_gbps, pkt_s,
        (unsigned long long)st.auth_ok,
        (unsigned long long)st.auth_fail,
        st.decrypt_fail ? "1;31m" : "0m",
        (unsigned long long)st.decrypt_fail,
        (unsigned long long)st.backpressure);
    std::fflush(stdout);
}

// ─── Final summary ──────────────────────────────────────────────────────────

inline void print_summary(const Samples& thr_samples,
                          double total_sec,
                          uint64_t total_bytes,
                          uint64_t total_pkts,
                          const gn::StatsSnapshot& st) {
    const bool has_errors = st.decrypt_fail > 0 || st.auth_fail > 0;
    const char* c_red   = "\033[1;31m";
    const char* c_green = "\033[1;32m";
    const char* c_none  = "";

    std::printf("\n\n");
    box_line(BOX_TL, BOX_TR);
    std::printf("%s                GoodNet Benchmark Results                 %s\n",
                BOX_V, BOX_V);

    auto sep = []() { box_line(BOX_ML, BOX_MR); };

    sep();

    print_row("Duration", fmt_duration(total_sec));
    print_row("Status", has_errors ? "DONE (errors!)" : "DONE (OK)",
              has_errors ? c_red : c_green);

    const double avg_gbps = thr_samples.mean();
    const double peak     = thr_samples.peak();

    sep();

    char avg_buf[128];
    std::snprintf(avg_buf, sizeof(avg_buf), "%.2f Gbps  %s",
                  avg_gbps, bar(avg_gbps / (peak > 0 ? peak : 1), 20).c_str());
    print_row("Average", avg_buf);
    print_row("Peak", (std::ostringstream() << std::fixed << std::setprecision(2)
                       << peak << " Gbps").str());
    print_row("p95", (std::ostringstream() << std::fixed << std::setprecision(2)
                      << thr_samples.percentile(95) << " Gbps").str());

    sep();

    uint64_t display_bytes = (total_bytes > 0) ? total_bytes : st.rx_bytes;
    uint64_t display_pkts  = (total_pkts > 0) ? total_pkts : st.rx_packets;

    print_row("Total data", fmt_bytes(static_cast<double>(display_bytes)));
    print_row("Total packets", fmt_num(display_pkts));

    const double pkt_s_avg = total_sec > 0 ? display_pkts / total_sec : 0;
    print_row("Avg pkt/s", fmt_num(static_cast<uint64_t>(pkt_s_avg)));

    sep();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%u / %u",
                  (unsigned)st.total_conn, (unsigned)st.total_disc);
    print_row("Connections (OK/Err)", buf);

    std::snprintf(buf, sizeof(buf), "%llu / %llu",
                  (unsigned long long)st.auth_ok,
                  (unsigned long long)st.auth_fail);
    print_row("Auth (ok/fail)", buf);

    uint64_t df = st.decrypt_fail;
    print_row("Decrypt failures", fmt_num(df), df > 0 ? c_red : c_none);
    print_row("Backpressure drops", fmt_num(st.backpressure));

    // ── Histogram ────────────────────────────────────────────────────────────
    if (thr_samples.count >= 4) {
        sep();
        auto sv = thr_samples.sorted();
        const double lo = sv.front(), hi = sv.back();
        constexpr int BINS = 8;
        constexpr int BAR_MAX_WIDTH = 24;

        std::array<int, BINS> bins{};
        for (double v : sv) {
            const int b = hi > lo
                ? static_cast<int>((v - lo) / (hi - lo) * (BINS - 1)) : 0;
            ++bins[std::min(b, BINS - 1)];
        }

        for (int i = 0; i < BINS; ++i) {
            int num_blocks = static_cast<int>(
                bins[i] * BAR_MAX_WIDTH / (sv.empty() ? 1 : sv.size()));

            std::string hb;
            for (int j = 0; j < num_blocks; ++j) hb += "\u2588";
            std::string spaces;
            for (int j = 0; j < (BAR_MAX_WIDTH - num_blocks); ++j) spaces += " ";

            char label[64];
            double b_start = lo + (hi - lo) * i / BINS;
            double b_end   = lo + (hi - lo) * (i + 1) / BINS;
            std::snprintf(label, sizeof(label), "%5.1f-%-5.1f Gbps",
                          b_start, b_end);

            std::printf("%s  %s %s %s%s%s %3d%%  %s\n",
                        BOX_V, label, BOX_SEP,
                        hb.c_str(), spaces.c_str(), BOX_SEP,
                        static_cast<int>(100.0 * bins[i] / sv.size()),
                        BOX_V);
        }
    }

    box_line(BOX_BL, BOX_BR);
}

} // namespace cli
