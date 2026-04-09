#pragma once

#include <string>
#include <memory>
#include <alpha/ipc/ShmManager.hpp>
#include <alpha/ipc/RingBuffer.hpp>
#include <alpha/models/MarketModels.hpp>

namespace alpha::strategy {

class ShmReader {
public:
    ShmReader(const std::string& segment_name, const std::string& buffer_name);

    /**
     * @brief Polls for the next signal from the signal engine.
     * @return true if a signal was read, false if the queue was empty.
     */
    bool poll(alpha::models::Signal& signal);

    /**
     * @brief Block and wait for the SHM segment to become available.
     */
    void wait_for_attachment();

private:
    std::string segment_name_;
    std::string buffer_name_;
    std::unique_ptr<alpha::ipc::ShmManager> shm_manager_;
    alpha::ipc::SPSCRingBuffer<alpha::models::Signal, 16384>* ring_buffer_;
};

} // namespace alpha::strategy
