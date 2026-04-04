#pragma once

#include <string>
#include <memory>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>

namespace alpha::signal {

class ShmWriter {
public:
    ShmWriter(const std::string& segment_name, const std::string& buffer_name);
    
    /**
     * @brief Publishes a new signal to the SHM RingBuffer.
     */
    void publish(const alpha::models::Signal& signal);
    
    /**
     * @brief Create the SHM segment if not already present.
     */
    void initialize();

private:
    std::string segment_name_;
    std::string buffer_name_;
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<alpha::models::Signal, 16384>* ring_buffer_;
};

} // namespace alpha::signal
