#ifndef GOODNET_H
#define GOODNET_H

#include <stdint.h>
#include <stddef.h>
#include "../sdk/types.h"   /* propagation_t, conn_id_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gn_core_t gn_core_t;

typedef struct {
    const char* config_dir;
    const char* log_level;
    uint16_t    listen_port;
} gn_config_t;

typedef struct {
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_packets, tx_packets;
    uint64_t auth_ok, auth_fail;
    uint64_t decrypt_fail, backpressure;
    uint64_t consumed, rejected;
    uint32_t connections, total_conn, total_disc;
    uint64_t drops[12];         ///< indexed by DropReason
    uint64_t dispatch_lat_avg;  ///< average dispatch latency, nanoseconds
} gn_stats_t;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

gn_core_t* gn_core_create    (gn_config_t* cfg);
void       gn_core_destroy   (gn_core_t* core);
void       gn_core_run       (gn_core_t* core);
void       gn_core_run_async (gn_core_t* core, int threads);
void       gn_core_stop      (gn_core_t* core);
int        gn_core_reload_config(gn_core_t* core);

// ── Network ───────────────────────────────────────────────────────────────────

void gn_core_send      (gn_core_t* core, const char* uri, uint32_t type,
                         const void* data, size_t len);
void gn_core_broadcast (gn_core_t* core, uint32_t type,
                         const void* data, size_t len);
void gn_core_disconnect(gn_core_t* core, uint64_t conn_id);
int  gn_core_rekey     (gn_core_t* core, uint64_t conn_id);

// ── Identity ──────────────────────────────────────────────────────────────────

size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len);

// ── Stats ─────────────────────────────────────────────────────────────────────

void gn_core_get_stats(gn_core_t* core, gn_stats_t* out);

// ── Subscriptions ─────────────────────────────────────────────────────────────

typedef propagation_t (*gn_handler_fn)(uint32_t type, const void* data,
                                        size_t len, void* user_data);

uint64_t gn_core_subscribe  (gn_core_t* core, uint32_t type,
                               gn_handler_fn cb, void* user_data);
void     gn_core_unsubscribe(gn_core_t* core, uint64_t sub_id);

#ifdef __cplusplus
}
#endif

#endif // GOODNET_H