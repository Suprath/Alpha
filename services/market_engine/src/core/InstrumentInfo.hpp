#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace alpha::market {

/**
 * @brief Segment classification for Indian markets.
 * Determines applicable tax schedule and margin requirements.
 */
enum class Segment : int32_t {
    EQUITY_INTRADAY = 1, // MIS — same-day squared off, 5x leverage
    EQUITY_DELIVERY = 2, // CNC — held overnight, full capital
    INDEX_FUTURES   = 3, // NIFTY/BANKNIFTY/etc. futures
    STOCK_FUTURES   = 4, // Single-stock futures
    INDEX_OPTIONS   = 5, // NIFTY/BANKNIFTY options (CE/PE)
    STOCK_OPTIONS   = 6, // Single-stock options
    CURRENCY_FUT    = 7, // USD-INR, EUR-INR, etc.
    CURRENCY_OPT    = 8,
    COMMODITY_FUT   = 9,
};

enum class OptionType : int32_t { NONE = 0, CALL = 1, PUT = -1 };

/**
 * @brief Static metadata for a single instrument.
 */
struct InstrumentInfo {
    uint32_t    token;
    char        symbol[32];
    Segment     segment;
    double      lot_size;     // 1 for equity; contract lot size for F&O
    double      tick_size;    // Minimum price movement
    double      strike;       // 0.0 for non-options
    OptionType  option_type;
    uint64_t    expiry_ns;    // 0 for equity
};

/**
 * @brief Infers segment from an NSE symbol string (Upstox format).
 *
 * Examples (Upstox):
 *   "RELIANCE"          → EQUITY_INTRADAY
 *   "NIFTY24DECFUT"     → INDEX_FUTURES
 *   "NIFTY24DEC24500CE" → INDEX_OPTIONS
 *   "RELIANCE24DECFUT"  → STOCK_FUTURES
 *   "RELIANCE24DEC3000CE"→ STOCK_OPTIONS
 */
inline Segment infer_segment(std::string_view sym) {
    // Options: ends in CE or PE
    if (sym.size() >= 2) {
        auto tail2 = sym.substr(sym.size() - 2);
        if (tail2 == "CE" || tail2 == "PE") {
            // Index option if symbol starts with NIFTY/BANKNIFTY/FINNIFTY
            if (sym.find("NIFTY") != std::string_view::npos ||
                sym.find("SENSEX") != std::string_view::npos) {
                return Segment::INDEX_OPTIONS;
            }
            return Segment::STOCK_OPTIONS;
        }
    }
    // Futures: contains "FUT"
    if (sym.find("FUT") != std::string_view::npos) {
        if (sym.find("NIFTY") != std::string_view::npos ||
            sym.find("SENSEX") != std::string_view::npos) {
            return Segment::INDEX_FUTURES;
        }
        return Segment::STOCK_FUTURES;
    }
    // Default: equity intraday (algorithmic strategies square off intraday)
    return Segment::EQUITY_INTRADAY;
}

/**
 * @brief In-memory instrument registry.
 * Pre-populated with common NSE F&O instruments; equity tokens added on first sight.
 */
class InstrumentRegistry {
public:
    static InstrumentRegistry& instance() {
        static InstrumentRegistry reg;
        return reg;
    }

    /**
     * @brief Look up or auto-register an instrument from an OrderIntent.
     */
    const InstrumentInfo& get_or_register(uint32_t token, const char* symbol) {
        auto it = registry_.find(token);
        if (it != registry_.end()) return it->second;

        InstrumentInfo info{};
        info.token = token;
        std::snprintf(info.symbol, sizeof(info.symbol), "%s", symbol);
        info.segment     = infer_segment(symbol);
        info.option_type = OptionType::NONE;
        info.expiry_ns   = 0;
        info.strike      = 0.0;

        // Default lot sizes for well-known index contracts
        std::string_view sym(symbol);
        if      (sym.find("BANKNIFTY") != std::string_view::npos) info.lot_size = 15;
        else if (sym.find("FINNIFTY")  != std::string_view::npos) info.lot_size = 40;
        else if (sym.find("NIFTY")     != std::string_view::npos) info.lot_size = 50;
        else if (sym.find("SENSEX")    != std::string_view::npos) info.lot_size = 10;
        else if (info.segment == Segment::STOCK_FUTURES ||
                 info.segment == Segment::STOCK_OPTIONS)          info.lot_size = 250; // common default
        else                                                       info.lot_size = 1.0;

        info.tick_size = (info.segment == Segment::INDEX_OPTIONS ||
                          info.segment == Segment::STOCK_OPTIONS) ? 0.05 : 0.05;

        registry_.emplace(token, info);
        return registry_.at(token);
    }

private:
    std::unordered_map<uint32_t, InstrumentInfo> registry_;
};

} // namespace alpha::market
