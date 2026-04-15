/**
 * Alpha Ingester v2 — Redis Stream → Shared Memory Bridge
 * ─────────────────────────────────────────────────────────────────────────────
 * Reads tick dicts published by the Python data_feed (live_feed.py) from the
 * Redis Stream "alpha:ticks" and pushes them as Tick structs into the POSIX
 * shared memory ring buffer consumed by the Signal Engine.
 *
 * This replaces the prior monolithic C++ Upstox WebSocket + protobuf client
 * with a thin, dependency-light bridge.  All Upstox protocol complexity now
 * lives in the Python data_feed service.
 *
 * Data flow:
 *   Python live_feed.py → XADD alpha:ticks → [this process] → SHM ring buffer
 *                                                                     ↓
 *                                                            Signal Engine (C++)
 */

#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

#include <hiredis/hiredis.h>

#include <alpha/config/Config.hpp>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::models;
using namespace alpha::ipc;

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Redis field helpers ───────────────────────────────────────────────────────

/** Return the value for `key` in a flat Redis reply array [k, v, k, v, ...]. */
static const char* field_val(redisReply* arr, const char* key) {
    for (size_t i = 0; i + 1 < arr->elements; i += 2) {
        if (arr->element[i]->str && std::strcmp(arr->element[i]->str, key) == 0) {
            return arr->element[i + 1]->str;
        }
    }
    return nullptr;
}

static inline double   r_dbl(redisReply* a, const char* k) { auto* s = field_val(a, k); return s ? std::strtod(s, nullptr)                    : 0.0; }
static inline uint64_t r_u64(redisReply* a, const char* k) { auto* s = field_val(a, k); return s ? static_cast<uint64_t>(std::strtoull(s, nullptr, 10)) : 0ULL; }
static inline uint32_t r_u32(redisReply* a, const char* k) { auto* s = field_val(a, k); return s ? static_cast<uint32_t>(std::strtoul(s, nullptr, 10))  : 0U; }

// ── Tick deserialisation ──────────────────────────────────────────────────────

static Tick parse_tick(redisReply* fields) {
    Tick t{};
    t.timestamp_ns     = r_u64(fields, "ts_ns");
    t.instrument_token = r_u32(fields, "token");
    t.last_price       = r_dbl(fields, "price");
    t.total_volume     = r_u64(fields, "volume");
    t.open_interest    = r_dbl(fields, "oi");
    t.bid_price        = r_dbl(fields, "bid_price");
    t.bid_size         = r_u32(fields, "bid_size");
    t.ask_price        = r_dbl(fields, "ask_price");
    t.ask_size         = r_u32(fields, "ask_size");

    // 5-level market depth
    for (int i = 0; i < 5; ++i) {
        std::string bp = "bid_p" + std::to_string(i);
        std::string bq = "bid_q" + std::to_string(i);
        std::string ap = "ask_p" + std::to_string(i);
        std::string aq = "ask_q" + std::to_string(i);
        t.bids[i].price    = r_dbl(fields, bp.c_str());
        t.bids[i].quantity = r_u32(fields, bq.c_str());
        t.asks[i].price    = r_dbl(fields, ap.c_str());
        t.asks[i].quantity = r_u32(fields, aq.c_str());
    }

    // Option greeks (0.0 for non-options)
    t.greeks.delta = r_dbl(fields, "delta");
    t.greeks.gamma = r_dbl(fields, "gamma");
    t.greeks.theta = r_dbl(fields, "theta");
    t.greeks.vega  = r_dbl(fields, "vega");
    t.greeks.rho   = r_dbl(fields, "rho");
    t.greeks.iv    = r_dbl(fields, "iv");

    return t;
}

// ── Redis reconnect helper ────────────────────────────────────────────────────

static redisContext* redis_connect(const std::string& host, int port) {
    redisContext* ctx = redisConnect(host.c_str(), port);
    if (!ctx) {
        std::cerr << "[Ingester] Redis OOM\n";
        return nullptr;
    }
    if (ctx->err) {
        std::cerr << "[Ingester] Redis connect error: " << ctx->errstr << "\n";
        redisFree(ctx);
        return nullptr;
    }
    std::cout << "[Ingester] Redis connected at " << host << ":" << port << "\n";
    return ctx;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Force line-buffered stdout so docker logs sees output immediately
    std::cout << std::unitbuf;

    std::cout << "=== Alpha Ingester v2.0 (Redis→SHM Bridge) ===" << std::endl;

    // ── SHM ring buffer (producer side) ──────────────────────────────────────
    auto shm_manager = std::make_unique<ShmManager>(
        "alpha_upstox_shm_v2", ShmRole::PRODUCER);
    auto* ring_buffer = shm_manager->get_or_create_buffer<
        SPSCRingBuffer<Tick, 65536>>("tick_queue");
    std::cout << "[Ingester] SHM ring buffer ready\n";

    // ── Redis ─────────────────────────────────────────────────────────────────
    const std::string redis_host =
        alpha::config::Config::get().get_string("REDIS_HOST", "redis");
    const int redis_port =
        alpha::config::Config::get().get_int("REDIS_PORT", 6379);

    redisContext* ctx = nullptr;
    while (g_running && !ctx) {
        ctx = redis_connect(redis_host, redis_port);
        if (!ctx) {
            std::cerr << "[Ingester] Retrying Redis in 3s…\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    if (!ctx) return 1;

    // ── Heartbeat thread (100 ms) ─────────────────────────────────────────────
    std::thread heartbeat([&]() {
        while (g_running.load(std::memory_order_relaxed)) {
            ring_buffer->heartbeat_ts_ns.store(
                alpha::time::Timestamp::now_ns(), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ── XREAD loop ────────────────────────────────────────────────────────────
    const std::string stream  = "alpha:ticks";
    std::string       last_id = "$";   // start from new messages only
    uint64_t          total   = 0;

    std::cout << "[Ingester] Reading from Redis stream " << stream << " …\n";

    while (g_running.load(std::memory_order_relaxed)) {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx,
                "XREAD COUNT 100 BLOCK 1000 STREAMS %s %s",
                stream.c_str(), last_id.c_str())
        );

        if (!reply) {
            std::cerr << "[Ingester] Redis error: " << ctx->errstr
                      << " — reconnecting…\n";
            redisFree(ctx);
            ctx = nullptr;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            while (g_running && !ctx) {
                ctx = redis_connect(redis_host, redis_port);
                if (!ctx)
                    std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            last_id = "$";   // restart from latest after reconnect
            continue;
        }

        // Timeout (nil) or empty — just loop
        if (reply->type == REDIS_REPLY_NIL || reply->elements == 0) {
            freeReplyObject(reply);
            continue;
        }

        // reply = [ [stream_name, [ [id, [k,v,...]], ... ]] ]
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0) {
            redisReply* stream_entry = reply->element[0];
            if (stream_entry->elements >= 2) {
                redisReply* messages = stream_entry->element[1];
                for (size_t m = 0; m < messages->elements; ++m) {
                    redisReply* msg = messages->element[m];
                    if (msg->elements < 2) continue;

                    last_id = msg->element[0]->str;   // advance cursor

                    Tick tick = parse_tick(msg->element[1]);
                    if (tick.instrument_token == 0) continue;  // malformed

                    ring_buffer->push(tick);
                    ++total;

                    if (total <= 5 || total % 500 == 0) {
                        std::cout << "[Ingester] tick#" << total
                                  << " token=" << tick.instrument_token
                                  << " price=" << tick.last_price
                                  << " vol=" << tick.total_volume
                                  << std::endl;
                    }
                }
            }
        }
        freeReplyObject(reply);
    }

    std::cout << "[Ingester] Shutdown — " << total << " total ticks\n";
    heartbeat.join();
    if (ctx) redisFree(ctx);
    return 0;
}
