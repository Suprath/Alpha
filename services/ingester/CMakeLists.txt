cmake_minimum_required(VERSION 3.20)
project(AlphaIngester VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# hiredis — C Redis client for XREAD
pkg_check_modules(hiredis REQUIRED hiredis)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(include)
include_directories(../../shared/cpp/include)
include_directories(${hiredis_INCLUDE_DIRS})

# ── Ingester binary (Redis → SHM bridge) ─────────────────────────────────────
add_executable(alpha-ingester src/main.cpp)

target_link_libraries(alpha-ingester
    Boost::system
    Threads::Threads
    ${hiredis_LIBRARIES}
)

target_compile_options(alpha-ingester PRIVATE
    -O2
    -Wall
    -Wextra
    -Werror
)

# ── Unit tests ────────────────────────────────────────────────────────────────
# Tests cover shared infrastructure (RingBuffer, MarketModels, RateLimiter).
# Upstox-specific protobuf/WebSocket tests have moved to services/data_feed.
enable_testing()
find_package(GTest REQUIRED)

add_executable(alpha-ingester-test
    ../../tests/unit/ingester_normalization_test.cpp
    ../../tests/unit/rate_limiter_test.cpp
    ../../tests/unit/ipc_ringbuffer_test.cpp
)

target_link_libraries(alpha-ingester-test
    GTest::gtest_main
    Threads::Threads
    Boost::system
)

add_test(NAME IngesterUnitTests COMMAND alpha-ingester-test)
