cmake_minimum_required(VERSION 3.20)
project(alpha-strategy-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Shared headers (MarketModels, RingBuffer, ShmManager, Timestamp, etc.)
include_directories(../../shared/cpp/include)

# Boost (Interprocess) — required by ShmManager
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Main binary
add_executable(alpha-strategy-engine
    src/main.cpp
    src/shm_reader/ShmReader.cpp
    src/shm_writer/OrderShmWriter.cpp
    src/strategy/StrategyEngine.cpp
)

target_include_directories(alpha-strategy-engine PRIVATE
    src
    src/shm_reader
    src/shm_writer
    src/strategy
    src/core
    ../../shared/cpp/include
)

target_link_libraries(alpha-strategy-engine PRIVATE
    pthread
    rt
)

target_compile_options(alpha-strategy-engine PRIVATE
    -O3
    -Wall
    -Wextra
)
