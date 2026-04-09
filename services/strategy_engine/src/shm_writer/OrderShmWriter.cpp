#include "OrderShmWriter.hpp"
#include <iostream>

namespace alpha::strategy {

OrderShmWriter::OrderShmWriter(const std::string& segment_name, const std::string& buffer_name)
    : segment_name_(segment_name), buffer_name_(buffer_name), ring_buffer_(nullptr) {
    shm_manager_ = std::make_unique<alpha::ipc::ShmManager>(segment_name, alpha::ipc::ShmRole::PRODUCER);
}

void OrderShmWriter::initialize() {
    ring_buffer_ = shm_manager_->get_or_create_buffer<
        alpha::ipc::SPSCRingBuffer<alpha::models::OrderIntent, 4096>>(buffer_name_);
    std::cout << "[OrderShmWriter] Initialized " << segment_name_ << "/" << buffer_name_ << "\n";
}

void OrderShmWriter::publish(const OrderIntent& intent) {
    if (!ring_buffer_) return;

    // Convert local POD OrderIntent to shared alpha::models::OrderIntent
    // Both structs use int32_t-backed enum fields — field-by-field copy is safe.
    alpha::models::OrderIntent shared{};
    shared.timestamp_ns     = intent.timestamp_ns;
    shared.instrument_token = intent.instrument_token;
    shared.price            = intent.price;
    shared.qty              = intent.qty;
    shared.side             = static_cast<int32_t>(intent.side);
    shared.reason           = static_cast<int32_t>(intent.reason);
    shared.confidence       = intent.confidence;
    shared.strategy_id      = intent.strategy_id;
    __builtin_memcpy(shared.symbol, intent.symbol, sizeof(shared.symbol));

    ring_buffer_->push(shared);
}

} // namespace alpha::strategy
