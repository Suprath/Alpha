#pragma once

#include <string>
#include <memory>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>
#include "../core/OrderIntent.hpp"

namespace alpha::strategy {

/**
 * @brief Publishes OrderIntent decisions to the market engine via shared memory.
 *
 * Segment: alpha_order_shm_v1 / order_queue
 * Capacity: 4096 orders (low frequency — ~1 per bar per instrument)
 */
class OrderShmWriter {
public:
    OrderShmWriter(const std::string& segment_name, const std::string& buffer_name);

    void initialize();
    void publish(const OrderIntent& intent);

private:
    std::string segment_name_;
    std::string buffer_name_;
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<alpha::models::OrderIntent, 4096>* ring_buffer_;
};

} // namespace alpha::strategy
