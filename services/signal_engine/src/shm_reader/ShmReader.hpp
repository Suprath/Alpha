#pragma once

#include <string>
#include <memory>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>

namespace alpha::signal {

class ShmReader {
public:
    ShmReader(const std::string& segment_name, const std::string& buffer_name);
    
    /**
     * @brief Polls for the next tick from the Ingester.
     * @return true if a tick was read, false otherwise.
     */
    bool poll(alpha::models::Tick& tick);
    
    /**
     * @brief Block and wait for the SHM segment to become available.
     */
    void wait_for_attachment();

private:
    std::string segment_name_;
    std::string buffer_name_;
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<alpha::models::Tick, 65536>* ring_buffer_;
};

} // namespace alpha::signal
