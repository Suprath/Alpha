cmake_minimum_required(VERSION 3.20)
project(alpha-market-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared headers (MarketModels, RingBuffer, ShmManager, Timestamp, etc.)
include_directories(../../shared/cpp/include)

# Boost (Interprocess) — required by ShmManager
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Main binary
add_executable(alpha-market-engine
    src/main.cpp
    src/shm_reader/ShmReader.cpp
    src/portfolio/PortfolioManager.cpp
    src/execution/ExecutionSimulator.cpp
    src/engine/MarketEngine.cpp
)

target_include_directories(alpha-market-engine PRIVATE
    src
    src/shm_reader
    src/core
    src/portfolio
    src/execution
    src/engine
    ../../shared/cpp/include
)

target_link_libraries(alpha-market-engine PRIVATE
    pthread
    rt
)

target_compile_options(alpha-market-engine PRIVATE
    -O3
    -Wall
    -Wextra
)
