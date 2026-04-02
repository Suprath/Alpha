#include <alpha/ingester/IngesterFactory.hpp>
#include <alpha/ingester/UpstoxIngester.hpp>
#include <alpha/config/Config.hpp>
#include <iostream>
#include <algorithm>

namespace alpha::ingester {

std::unique_ptr<DataIngester> IngesterFactory::create(boost::asio::io_context& ioc, const std::string& vendor) {
    std::string v = vendor;
    std::transform(v.begin(), v.end(), v.begin(), ::toupper);

    if (v == "UPSTOX") {
        return std::make_unique<UpstoxIngester>(ioc);
    } 
    
    // Future vendors can be added here easily:
    // else if (v == "ZERODHA") { return std::make_unique<ZerodhaIngester>(ioc); }
    
    std::cerr << "[Factory] Unknown vendor requested: " << vendor << ". Defaulting to UPSTOX." << std::endl;
    return std::make_unique<UpstoxIngester>(ioc);
}

} // namespace alpha::ingester
