cmake_minimum_required(VERSION 3.20)
project(alpha-market-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared headers (MarketModels, RingBuffer, ShmManager, Timestamp, etc.)
include_directories(../../shared/cpp/include)

# Boost (Interprocess) — required by ShmManager
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# hiredis — required by RedisPublisher
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIB hiredis)
include_directories(${HIREDIS_INCLUDE_DIR})

# Protobuf — required by RedisPublisher (portfolio/trade binary serialization)
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})  # for alpha_portfolio.pb.h

# ── Proto generation ──────────────────────────────────────────────────────────
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/proto")

add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/alpha_portfolio.pb.cc"
            "${CMAKE_CURRENT_BINARY_DIR}/alpha_portfolio.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            "--proto_path=${PROTO_DIR}"
            "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
            "${PROTO_DIR}/alpha_portfolio.proto"
    DEPENDS "${PROTO_DIR}/alpha_portfolio.proto"
    COMMENT "Generating C++ bindings from alpha_portfolio.proto"
)

# Main binary
add_executable(alpha-market-engine
    src/main.cpp
    src/shm_reader/ShmReader.cpp
    src/portfolio/PortfolioManager.cpp
    src/execution/ExecutionSimulator.cpp
    src/engine/MarketEngine.cpp
    src/redis/RedisPublisher.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/alpha_portfolio.pb.cc"
)

target_include_directories(alpha-market-engine PRIVATE
    src
    src/shm_reader
    src/core
    src/portfolio
    src/execution
    src/engine
    ../../shared/cpp/include
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(alpha-market-engine PRIVATE
    pthread
    rt
    ${HIREDIS_LIB}
    ${Protobuf_LIBRARIES}
)

target_compile_options(alpha-market-engine PRIVATE
    -O3
    -Wall
    -Wextra
)
