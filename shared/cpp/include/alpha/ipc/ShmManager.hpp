#pragma once

#include <string>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <alpha/time/Timestamp.hpp>

namespace alpha::ipc {

enum class ShmRole { PRODUCER, CONSUMER };

/**
 * @brief Managed mapping wrapper for /dev/shm IPC.
 * Features automated Zombie segment destruction if health heartbeats are stale.
 */
class ShmManager {
public:
    ShmManager(const std::string& segment_name, ShmRole role, size_t shm_size = 64 * 1024 * 1024) 
        : segment_name_(segment_name), role_(role), shm_size_(shm_size) {}

    /**
     * @brief Get or create the RingBuffer structure inside the IPC memory map.
     */
    template <typename BufferType>
    BufferType* get_or_create_buffer(const std::string& buffer_name) {
        if (role_ == ShmRole::PRODUCER) {
            bool stale = check_zombie<BufferType>(buffer_name);
            if (stale) {
                std::cout << "[IPC] Zombie segment detected for " << segment_name_ << ". Wiping and recreating." << std::endl;
                boost::interprocess::shared_memory_object::remove(segment_name_.c_str());
            }

            shm_ = std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::open_or_create, segment_name_.c_str(), shm_size_
            );
            
            return shm_->find_or_construct<BufferType>(buffer_name.c_str())();
        } else {
            shm_ = std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::open_only, segment_name_.c_str()
            );
            
            auto res = shm_->find<BufferType>(buffer_name.c_str());
            if (!res.first) {
                throw std::runtime_error("Consumer could not find buffer: " + buffer_name);
            }
            return res.first;
        }
    }

private:
    std::string segment_name_;
    ShmRole role_;
    size_t shm_size_;
    std::unique_ptr<boost::interprocess::managed_shared_memory> shm_;

    template <typename BufferType>
    bool check_zombie(const std::string& buffer_name) {
        try {
            boost::interprocess::managed_shared_memory existing(boost::interprocess::open_only, segment_name_.c_str());
            auto res = existing.find<BufferType>(buffer_name.c_str());
            if (res.first) {
                uint64_t last_hb = res.first->heartbeat_ts_ns.load(std::memory_order_relaxed);
                // If it's been starting up for the first time, heartbeat might be 0.
                if (last_hb == 0) return true;
                
                uint64_t now = alpha::time::Timestamp::now_ns();
                // Flag as zombie if heartbeat is older than 5 seconds
                if ((now >= last_hb) && (now - last_hb > 5000000000ULL)) {
                    return true;
                }
            }
        } catch (const boost::interprocess::interprocess_exception& e) {
            if (e.get_error_code() == boost::interprocess::not_found_error) {
                // Segment doesn't exist yet, so it's not a zombie
                return false;
            }
            // Some other corruption occurred, treat as zombie to wipe it
            return true;
        } catch (...) {
            // General failure (segment corrupted or locked), kill it
            return true;
        }
        return false;
    }
};

} // namespace alpha::ipc
