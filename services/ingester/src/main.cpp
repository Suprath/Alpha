/**
 * Alpha Ingester — Redis Stream → Shared Memory Bridge
 * ─────────────────────────────────────────────────────────────────────────────
 * Reads alpha.feed.Tick protobuf messages published by the Python data_feed
 * service from the Redis Stream "alpha:ticks" and pushes them as Tick structs
 * into the POSIX shared memory ring buffer consumed by the Signal Engine.
 *
 * Data flow:
 *   Python live_feed.py
 *     → XADD alpha:ticks * data <serialized alpha.feed.Tick bytes>
 *       → [this process] deserialises proto → maps to models::Tick
 *         → SHM ring buffer
 *           → Signal Engine (C++)
 *
 * Schema contract: shared/proto/alpha_tick.proto
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

#include "alpha_tick.pb.h"               // generated from shared/proto/alpha_tick.proto

#include <alpha/config/Config.hpp>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>
#include <alpha/time/Timestamp.hpp>

using namespace alpha::models;
using namespace alpha::ipc;

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Redis binary field helper ─────────────────────────────────────────────────

/**
 * Find field `key` in a flat Redis reply array [k, v, k, v, ...] and return
 * a pointer + byte length to the value. Returns false if not found.
 *
 * IMPORTANT: use `out_len`, not strlen(), when passing to protobuf ParseFromArray.
 * Binary proto payloads may contain embedded null bytes.
 */
static bool field_bin(redisReply* arr, const char* key,
                      const char*& out_ptr, size_t& out_len) {
    for (size_t i = 0; i + 1 < arr->elements; i += 2) {
        if (arr->element[i]->str &&
            std::strcmp(arr->element[i]->str, key) == 0) {
            out_ptr = arr->element[i + 1]->str;
            out_len = arr->element[i + 1]->len;
            return true;
        }
    }
    return false;
}

// ── Proto → internal Tick mapping ────────────────────────────────────────────

static Tick proto_to_tick(const alpha::feed::Tick& p) {
    Tick t{};
    t.timestamp_ns     = p.timestamp_ns();
    t.instrument_token = p.token();
    t.last_price       = p.last_price();
    t.total_volume     = p.volume();
    t.open_interest    = p.open_interest();
    t.bid_price        = p.bid_price();
    t.bid_size         = p.bid_size();
    t.ask_price        = p.ask_price();
    t.ask_size         = p.ask_size();

    const int bid_depth = std::min(5, p.bids_size());
    for (int i = 0; i < bid_depth; ++i) {
        t.bids[i].price    = p.bids(i).price();
        t.bids[i].quantity = p.bids(i).quantity();
        t.bids[i].orders   = p.bids(i).orders();
    }
    const int ask_depth = std::min(5, p.asks_size());
    for (int i = 0; i < ask_depth; ++i) {
        t.asks[i].price    = p.asks(i).price();
        t.asks[i].quantity = p.asks(i).quantity();
        t.asks[i].orders   = p.asks(i).orders();
    }

    // greeks() always returns a valid (possibly zero-filled) OptionGreeks message
    t.greeks.delta = p.greeks().delta();
    t.greeks.gamma = p.greeks().gamma();
    t.greeks.theta = p.greeks().theta();
    t.greeks.vega  = p.greeks().vega();
    t.greeks.rho   = p.greeks().rho();
    t.greeks.iv    = p.greeks().iv();

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

    std::cout << "=== Alpha Ingester (Redis → SHM, proto transport) ===\n";

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
    uint64_t          parse_errors = 0;

    std::cout << "[Ingester] Reading proto ticks from Redis stream " << stream << " …\n";

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

                    // ── Deserialise proto payload ─────────────────────────────
                    const char* pb_data = nullptr;
                    size_t      pb_len  = 0;
                    if (!field_bin(msg->element[1], "data", pb_data, pb_len)) {
                        std::cerr << "[Ingester] Stream entry missing 'data' field\n";
                        ++parse_errors;
                        continue;
                    }

                    alpha::feed::Tick proto_tick;
                    if (!proto_tick.ParseFromArray(pb_data, static_cast<int>(pb_len))) {
                        std::cerr << "[Ingester] Proto parse failed (" << pb_len << " bytes)\n";
                        ++parse_errors;
                        continue;
                    }

                    if (proto_tick.token() == 0) continue;  // skip unresolved instruments

                    Tick tick = proto_to_tick(proto_tick);
                    ring_buffer->push(tick);
                    ++total;

                    if (total <= 5 || total % 500 == 0) {
                        std::cout << "[Ingester] tick#" << total
                                  << " token=" << tick.instrument_token
                                  << " price=" << tick.last_price
                                  << " vol=" << tick.total_volume
                                  << (parse_errors ? " errs=" + std::to_string(parse_errors) : "")
                                  << std::endl;
                    }
                }
            }
        }
        freeReplyObject(reply);
    }

    std::cout << "[Ingester] Shutdown — " << total << " ticks, "
              << parse_errors << " parse errors\n";
    heartbeat.join();
    if (ctx) redisFree(ctx);
    return 0;
}
