#include "ShmReader.hpp"
#include <thread>
#include <chrono>
#include <iostream>

namespace alpha::market {

ShmReader::ShmReader(const std::string& segment_name, const std::string& buffer_name)
    : segment_name_(segment_name), buffer_name_(buffer_name), ring_buffer_(nullptr) {
    shm_manager_ = std::make_unique<alpha::ipc::ShmManager>(segment_name, alpha::ipc::ShmRole::CONSUMER);
}

void ShmReader::wait_for_attachment() {
    std::cout << "[ShmReader] Waiting for " << segment_name_ << "/" << buffer_name_ << "..." << std::endl;
    while (!ring_buffer_) {
        try {
            ring_buffer_ = shm_manager_->get_or_create_buffer<
                alpha::ipc::SPSCRingBuffer<alpha::models::OrderIntent, 4096>>(buffer_name_);
            std::cout << "[ShmReader] Attached to order SHM segment." << std::endl;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

bool ShmReader::poll(alpha::models::OrderIntent& intent) {
    if (!ring_buffer_) {
        try {
            ring_buffer_ = shm_manager_->get_or_create_buffer<
                alpha::ipc::SPSCRingBuffer<alpha::models::OrderIntent, 4096>>(buffer_name_);
            std::cout << "[ShmReader] Attached to order SHM segment." << std::endl;
        } catch (...) {
            return false;
        }
    }
    return ring_buffer_->pop(intent);
}

} // namespace alpha::market
