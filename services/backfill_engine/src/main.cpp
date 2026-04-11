#include <alpha/backfill/UpstoxHistoricalFeed.hpp>
#include <alpha/backfill/SlidingWindowThrottler.hpp>
#include <alpha/backfill/TickBuffer.hpp>
#include <alpha/backfill/QuestDBILPWriter.hpp>

#include <pqxx/pqxx>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <ctime>

using namespace alpha;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string get_env(const char* key, const char* default_val = "") {
    const char* val = std::getenv(key);
    return val ? val : default_val;
}

// Split a comma-separated string into a trimmed set of non-empty tokens.
static std::set<std::string> parse_csv_set(const std::string& s) {
    std::set<std::string> result;
    if (s.empty()) return result;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t start = tok.find_first_not_of(" \t\r\n");
        const size_t end   = tok.find_last_not_of(" \t\r\n");
        if (start != std::string::npos)
            result.insert(tok.substr(start, end - start + 1));
    }
    return result;
}

// Split a comma-separated string into an ordered vector.
static std::vector<std::string> parse_csv_vec(const std::string& s) {
    std::vector<std::string> result;
    if (s.empty()) return result;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t start = tok.find_first_not_of(" \t\r\n");
        const size_t end   = tok.find_last_not_of(" \t\r\n");
        if (start != std::string::npos)
            result.push_back(tok.substr(start, end - start + 1));
    }
    return result;
}

static std::string format_today() {
    char buf[12];
    const std::time_t now = std::time(nullptr);
    std::tm* t = std::gmtime(&now);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", t);
    return buf;
}

// bars_per_year for realized-vol annualization.
static double bars_per_year_for_interval(const std::string& iv) {
    if (iv == "1minute")                         return backfill::TickBuffer::BARS_PER_YEAR_1M; // 375×252
    if (iv == "5minute")                         return backfill::TickBuffer::BARS_PER_YEAR_5M; //  75×252
    if (iv == "15minute")                        return 25.0   * 252.0;
    if (iv == "30minute")                        return 12.5   * 252.0;
    if (iv == "60minute" || iv == "1hour")       return 6.0    * 252.0;
    return backfill::TickBuffer::BARS_PER_YEAR_1D;  // day / week / month
}

// ── Instrument loader ─────────────────────────────────────────────────────────

struct Instrument {
    int32_t     id;              // PK from instrument_universe — used as QDB token
    std::string symbol;          // trading_symbol
    std::string instrument_key;  // e.g. "NSE_EQ|INE002A01018"
};

/**
 * InstrumentFilter — applied as SQL WHERE clauses.
 * An empty set means "no restriction on that dimension".
 *
 * Env vars:
 *   BACKFILL_SYMBOLS          comma-separated trading_symbol list (e.g. "RELIANCE,NIFTY50")
 *   BACKFILL_SEGMENTS         comma-separated segment list        (e.g. "NSE_EQ,NSE_INDEX")
 *   BACKFILL_INSTRUMENT_TYPES comma-separated type list           (e.g. "EQ,INDEX")
 *   BACKFILL_LIMIT            max instruments to process (0 = all)
 */
struct InstrumentFilter {
    std::set<std::string> symbols;
    std::set<std::string> segments;
    std::set<std::string> instrument_types;
    int                   limit{0};
};

// Build a pqxx-quoted IN clause: " AND col IN ('a','b')"
static std::string build_in_clause(pqxx::work& txn,
                                   const std::string& col,
                                   const std::set<std::string>& vals) {
    if (vals.empty()) return {};
    std::string clause = " AND " + col + " IN (";
    bool first = true;
    for (const auto& v : vals) {
        if (!first) clause += ',';
        clause += txn.quote(v);
        first = false;
    }
    clause += ')';
    return clause;
}

static std::vector<Instrument> load_instruments(const std::string& dsn,
                                                 const InstrumentFilter& filter) {
    std::vector<Instrument> out;
    try {
        pqxx::connection conn(dsn);
        pqxx::work txn(conn);

        // Always use the most recent date's snapshot.
        std::string sql =
            "SELECT id, instrument_key, trading_symbol "
            "FROM instrument_universe "
            "WHERE date = (SELECT MAX(date) FROM instrument_universe)";

        sql += build_in_clause(txn, "trading_symbol", filter.symbols);
        sql += build_in_clause(txn, "segment",        filter.segments);
        sql += build_in_clause(txn, "instrument_type", filter.instrument_types);
        sql += " ORDER BY trading_symbol";

        if (filter.limit > 0)
            sql += " LIMIT " + std::to_string(filter.limit);

        for (const auto& row : txn.exec(sql)) {
            Instrument inst;
            inst.id             = row[0].as<int32_t>();
            inst.instrument_key = row[1].as<std::string>();
            inst.symbol         = row[2].is_null() ? "" : row[2].as<std::string>();
            out.push_back(inst);
        }
    } catch (const std::exception& e) {
        std::cerr << "[backfill] load_instruments error: " << e.what() << std::endl;
    }
    return out;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    // ── Credentials & connections ─────────────────────────────────────────
    const std::string access_token = get_env("UPSTOX_ACCESS_TOKEN");
    const std::string pg_host      = get_env("POSTGRES_HOST",     "postgres-db");
    const std::string pg_port      = get_env("POSTGRES_PORT",     "5432");
    const std::string pg_db        = get_env("POSTGRES_DB",       "alpha_db");
    const std::string pg_user      = get_env("POSTGRES_USER",     "alpha_user");
    const std::string pg_pass      = get_env("POSTGRES_PASSWORD", "alpha_password");
    const std::string qdb_host     = get_env("QUESTDB_HOST",      "questdb");
    const uint16_t    qdb_ilp_port = static_cast<uint16_t>(
                                         std::stoi(get_env("QUESTDB_ILP_PORT", "9009")));

    if (access_token.empty()) {
        std::cerr << "[backfill] FATAL: UPSTOX_ACCESS_TOKEN not set.\n";
        return 1;
    }

    // ── Backfill date range ───────────────────────────────────────────────
    const std::string from_date = get_env("BACKFILL_FROM_DATE", "2024-01-01");
    const std::string to_date   = get_env("BACKFILL_TO_DATE",   format_today().c_str());

    // ── Multi-timeframe: BACKFILL_INTERVALS takes precedence over BACKFILL_INTERVAL ──
    const std::string intervals_env = get_env("BACKFILL_INTERVALS",
                                              get_env("BACKFILL_INTERVAL", "1minute").c_str());
    const auto intervals = parse_csv_vec(intervals_env);
    if (intervals.empty()) {
        std::cerr << "[backfill] FATAL: No intervals configured.\n";
        return 1;
    }

    // ── Symbol filtering ──────────────────────────────────────────────────
    InstrumentFilter filter;
    filter.symbols          = parse_csv_set(get_env("BACKFILL_SYMBOLS"));
    filter.segments         = parse_csv_set(get_env("BACKFILL_SEGMENTS"));
    filter.instrument_types = parse_csv_set(get_env("BACKFILL_INSTRUMENT_TYPES"));
    filter.limit            = std::stoi(get_env("BACKFILL_LIMIT", "0"));

    std::cout << "[backfill] Intervals : ";
    for (size_t i = 0; i < intervals.size(); ++i)
        std::cout << (i ? "," : "") << intervals[i];
    std::cout << "\n[backfill] Date range: " << from_date << " → " << to_date << '\n';
    if (!filter.symbols.empty()) {
        std::cout << "[backfill] Symbols   : ";
        for (const auto& s : filter.symbols) std::cout << s << ' ';
        std::cout << '\n';
    }
    if (!filter.segments.empty()) {
        std::cout << "[backfill] Segments  : ";
        for (const auto& s : filter.segments) std::cout << s << ' ';
        std::cout << '\n';
    }
    if (!filter.instrument_types.empty()) {
        std::cout << "[backfill] Types     : ";
        for (const auto& s : filter.instrument_types) std::cout << s << ' ';
        std::cout << '\n';
    }
    if (filter.limit > 0)
        std::cout << "[backfill] Limit     : " << filter.limit << " instruments\n";

    // ── Load instruments from PostgreSQL ──────────────────────────────────
    const std::string pg_dsn =
        "host=" + pg_host + " port=" + pg_port +
        " dbname=" + pg_db + " user=" + pg_user + " password=" + pg_pass;

    const auto instruments = load_instruments(pg_dsn, filter);
    if (instruments.empty()) {
        std::cerr << "[backfill] No instruments matched filter. Exiting.\n";
        return 1;
    }
    std::cout << "[backfill] Instruments loaded: " << instruments.size() << '\n';

    // ── Infrastructure ────────────────────────────────────────────────────
    // Upstox free tier: 100 req/min. Use 80 for safety margin.
    backfill::SlidingWindowThrottler throttler(80, 60'000'000'000ULL);

    backfill::QuestDBILPWriter writer(qdb_host, qdb_ilp_port);
    writer.connect();
    std::cout << "[backfill] QuestDB ILP connected at "
              << qdb_host << ':' << qdb_ilp_port << '\n';

    backfill::UpstoxHistoricalFeed feed(access_token);

    // ── Main loop: instruments × intervals ───────────────────────────────
    uint64_t total_candles = 0;
    uint64_t inst_idx      = 0;

    for (const auto& inst : instruments) {
        ++inst_idx;
        std::cout << "[backfill] [" << inst_idx << '/' << instruments.size() << "] "
                  << inst.symbol << " (" << inst.instrument_key << ")\n";

        for (const auto& interval : intervals) {
            const double bars_py = bars_per_year_for_interval(interval);
            backfill::TickBuffer buffer(backfill::TickBuffer::DEFAULT_WINDOW, bars_py);

            backfill::HistoricalRequest req;
            req.instrument_key = inst.instrument_key;
            req.interval       = interval;
            req.from_date      = from_date;
            req.to_date        = to_date;

            const uint32_t token = static_cast<uint32_t>(inst.id);

            size_t count = 0;
            try {
                // fetch_range handles date chunking; throttler.acquire() is
                // called inside pre_fetch_cb — once per HTTP request, not per instrument.
                count = feed.fetch_range(req, token, inst.symbol,
                    [&](const models::Candle& candle, uint32_t tk, const std::string& sym) {
                        buffer.push(candle, tk, sym, interval,
                            [&](const backfill::PreCalcBar& bar) {
                                writer.write_candle(bar.candle, bar.instrument_token,
                                                    bar.symbol, bar.interval);
                                writer.write_precalc_bar(bar.candle, bar.instrument_token,
                                                         bar.symbol, bar.interval,
                                                         bar.ofi, bar.kyle_lambda,
                                                         bar.vwap, bar.realized_vol);
                            },
                            [&](const models::BacktestTick& bt, const std::string& s) {
                                writer.write_backtest_tick(bt, s);
                            }
                        );
                    },
                    [&]() { throttler.acquire(); }  // called before each HTTP chunk
                );
            } catch (const std::exception& e) {
                std::cerr << "[backfill] ERROR " << inst.symbol
                          << ' ' << interval << ": " << e.what() << '\n';
                continue;
            }

            writer.flush();
            total_candles += count;
            std::cout << "  [" << interval << "] " << count << " candles\n";
        }
    }

    writer.flush();
    std::cout << "[backfill] Done.\n"
              << "  Total candles : " << total_candles        << '\n'
              << "  QDB rows      : " << writer.rows_written() << '\n'
              << "  Bytes sent    : " << writer.bytes_sent()   << '\n';

    return 0;
}
