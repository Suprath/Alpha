cmake_minimum_required(VERSION 3.20)
project(alpha-signal-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Includes
include_directories(include)
include_directories(../../shared/cpp/include)

# Boost (Interprocess)
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# libpqxx — used by backtest-batch mode to read candles from QuestDB
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)
include_directories(${pqxx_INCLUDE_DIRS})

# Main Binary
add_executable(alpha-signal-engine
    src/main.cpp
    src/shm_reader/ShmReader.cpp
    src/shm_writer/ShmWriter.cpp
)

target_include_directories(alpha-signal-engine PRIVATE
    src
    src/shm_reader
    src/shm_writer
    src/core
    src/pipelines
    src/signals
    ../../shared/cpp/include
)

target_link_libraries(alpha-signal-engine PRIVATE
    pthread
    rt
    ${pqxx_LIBRARIES}
)

# Testing — all signal calculators are header-only; no extra .cpp sources needed
enable_testing()
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(alpha-signal-engine-test
    ../../tests/unit/signal_engine_test.cpp
)

target_include_directories(alpha-signal-engine-test PRIVATE
    src
    src/core
    src/pipelines
    src/signals
    src/signals/ofi
    src/signals/trade_direction
    src/signals/vpin
    src/signals/kyles_lambda
    src/signals/entropy
    src/signals/bar
    src/signals/options
    src/signals/derived
    ../../shared/cpp/include
)

target_link_libraries(alpha-signal-engine-test
    GTest::gtest_main
    Threads::Threads
)

target_compile_options(alpha-signal-engine-test PRIVATE
    -O2
    -Wall
    -Wextra
)

add_test(NAME SignalEngineUnitTests COMMAND alpha-signal-engine-test)
