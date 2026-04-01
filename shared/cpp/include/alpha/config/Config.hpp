#pragma once

#include <string>
#include <cstdlib>
#include <stdexcept>
#include <optional>

namespace alpha::config {

/**
 * @brief Thread-safe configuration loader for Alpha.
 * Reads attributes from the process environment.
 */
class Config {
public:
    static Config& get() {
        static Config instance;
        return instance;
    }

    /**
     * @brief Get an environment variable or throw if required.
     */
    std::string get_string(const std::string& key, std::optional<std::string> default_val = std::nullopt) {
        const char* val = std::getenv(key.c_str());
        if (!val) {
            if (default_val.has_value()) {
                return default_val.value();
            }
            throw std::runtime_error("Required environment variable not found: " + key);
        }
        return std::string(val);
    }

    /**
     * @brief Get an environment variable as an integer.
     */
    int get_int(const std::string& key, int default_val) {
        const char* val = std::getenv(key.c_str());
        if (!val) return default_val;
        return std::atoi(val);
    }

private:
    Config() = default;
};

} // namespace alpha::config
