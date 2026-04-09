#pragma once

#include <string>
#include <memory>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>

namespace alpha::market {

/**
 * @brief CONSUMER of OrderIntent structs published by the strategy engine.
 *
 * Segment: alpha_order_shm_v1 / order_queue  (4096-capacity SPSC ring buffer)
 */
class ShmReader {
public:
    ShmReader(const std::string& segment_name, const std::string& buffer_name);

    /**
     * @brief Polls for the next OrderIntent from the strategy engine.
     * @return true if an order was read, false if the queue was empty.
     */
    bool poll(alpha::models::OrderIntent& intent);

    /**
     * @brief Block until the SHM segment is available (strategy engine has started).
     */
    void wait_for_attachment();

private:
    std::string segment_name_;
    std::string buffer_name_;
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<alpha::models::OrderIntent, 4096>* ring_buffer_;
};

} // namespace alpha::market
