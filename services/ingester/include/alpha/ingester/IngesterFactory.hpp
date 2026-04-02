#pragma once

#include <memory>
#include <string>
#include <utility>
#include <boost/asio.hpp>
#include <alpha/ingester/DataIngester.hpp>

namespace alpha::ingester {

/**
 * @brief Factory for creating DataIngester instances.
 */
class IngesterFactory {
public:
    /**
     * @brief Create a concrete ingester based on the vendor name.
     * @param ioc Boost.Asio io_context for networking.
     * @param vendor Name of the vendor (e.g. "UPSTOX").
     * @return unique_ptr to the created ingester.
     */
    static std::unique_ptr<DataIngester> create(boost::asio::io_context& ioc, const std::string& vendor);
};

} // namespace alpha::ingester
