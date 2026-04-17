cmake_minimum_required(VERSION 3.20)
project(AlphaBacktestEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)
pkg_check_modules(HIREDIS REQUIRED hiredis)
find_package(Protobuf REQUIRED)

# Generate C++ bindings from alpha_backtest.proto
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    ../../shared/proto/alpha_backtest.proto
)

# ── Include paths ─────────────────────────────────────────────────────────
include_directories(include)
include_directories(../../shared/cpp/include)
include_directories(${pqxx_INCLUDE_DIRS})
include_directories(${HIREDIS_INCLUDE_DIRS})
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})   # For generated .pb.h files
# market_engine portfolio — cross-service include for PortfolioManager, ExecutionSimulator
include_directories(../../services/market_engine/src)

# ── Sources ───────────────────────────────────────────────────────────────
set(BACKTEST_SOURCES
    src/main.cpp
    src/TickLoader.cpp
    src/SimRunner.cpp
    src/ResultAggregator.cpp
    src/BacktestPortfolio.cpp
    # market_engine portfolio (header-only except PortfolioManager.cpp + ExecutionSimulator.cpp)
    ../../services/market_engine/src/portfolio/PortfolioManager.cpp
    ../../services/market_engine/src/execution/ExecutionSimulator.cpp
    # Generated protobuf sources
    ${PROTO_SRCS}
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(alpha-backtest ${BACKTEST_SOURCES})

target_link_libraries(alpha-backtest
    Threads::Threads
    ${pqxx_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    ${Protobuf_LIBRARIES}
)

# ── Optimization flags (HFT hot-loop) ────────────────────────────────────
# -O3 + -march=native enables AVX2/AVX-512 auto-vectorization on the tick loop
target_compile_options(alpha-backtest PRIVATE
    -O3
    -march=native
    -funroll-loops
    -ffast-math
    -Wall
    -Wextra
    -Werror
    -Wno-restrict   # Suppress false positive from libpqxx template instantiation
)

# ── Optional: link with jemalloc for better allocation performance ─────────
# find_library(JEMALLOC_LIB jemalloc)
# if(JEMALLOC_LIB)
#     target_link_libraries(alpha-backtest ${JEMALLOC_LIB})
# endif()

# ── Unit Tests ────────────────────────────────────────────────────────────────
enable_testing()
find_package(GTest REQUIRED)

add_executable(alpha-backtest-test
    ../../tests/unit/backtest_engine_test.cpp
    src/ResultAggregator.cpp
    src/SimRunner.cpp
    src/TickLoader.cpp
    src/BacktestPortfolio.cpp
    ../../services/market_engine/src/portfolio/PortfolioManager.cpp
    ../../services/market_engine/src/execution/ExecutionSimulator.cpp
    ${PROTO_SRCS}
)

target_include_directories(alpha-backtest-test PRIVATE
    include
    ../../shared/cpp/include
    ${pqxx_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
    ../../services/market_engine/src
)

target_link_libraries(alpha-backtest-test
    GTest::gtest_main
    Threads::Threads
    ${pqxx_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    ${Protobuf_LIBRARIES}
)

target_compile_options(alpha-backtest-test PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-stringop-truncation
)

add_test(NAME BacktestEngineUnitTests COMMAND alpha-backtest-test)
