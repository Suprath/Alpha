#include <iostream>
#include <alpha/concurrency/SpinLock.hpp>
#include <utility>
#include <alpha/time/Timestamp.hpp>
#include <boost/asio.hpp>
#include <utility>

using boost::asio::ip::tcp;

/**
 * @brief Alpha Ingester Service
 * Handles reliable TCP data ingestion for market data or order updates.
 * Optimized for C++20 and Boost.Asio.
 */
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    try {
        std::cout << "--- Alpha Ingester v0.1.0 ---" << std::endl;
        std::cout << "Starting TCP ingestion service at " << alpha::time::Timestamp::now_ns() << " ns" << std::endl;

        boost::asio::io_context io_context;

        // In a real HFT engine, we would pin this thread to a dedicated core.
        // We'll add thread affinity rules later.

        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 9000));

        std::cout << "Listening on port 9000..." << std::endl;

        // Dummy loop to keep the process alive for now
        // We will implement concurrent TCP session handling next.
        while (true) {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            
            std::cout << "New connection received at " << alpha::time::Timestamp::now_ns() << " ns" << std::endl;
            // Handle session...
        }

    } catch (std::exception& e) {
        std::cerr << "Exception in Ingester: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
