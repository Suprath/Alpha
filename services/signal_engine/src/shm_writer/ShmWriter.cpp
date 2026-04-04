#include "ShmWriter.hpp"
#include <iostream>

namespace alpha::signal {

ShmWriter::ShmWriter(const std::string& segment_name, const std::string& buffer_name)
    : segment_name_(segment_name), buffer_name_(buffer_name), ring_buffer_(nullptr) {
    // ShmRole::PRODUCER for the writer
    shm_manager_ = std::make_unique<alpha::ipc::ShmManager>(segment_name, alpha::ipc::ShmRole::PRODUCER);
}

void ShmWriter::initialize() {
    std::cout << "[ShmWriter] Initializing SHM segment: " << segment_name_ << "..." << std::endl;
    // Map or create the ring buffer. 
    // Capacity 16384 is plenty for signals (low frequency compared to ticks).
    ring_buffer_ = shm_manager_->get_or_create_buffer<alpha::ipc::SPSCRingBuffer<alpha::models::Signal, 16384>>(buffer_name_);
    
    if (ring_buffer_) {
        std::cout << "[ShmWriter] Successfully created/attached to SHM segment." << std::endl;
    } else {
        throw std::runtime_error("Failed to initialize ShmWriter buffer.");
    }
}

void ShmWriter::publish(const alpha::models::Signal& signal) {
    if (ring_buffer_) {
        ring_buffer_->push(signal);
    }
}

} // namespace alpha::signal
