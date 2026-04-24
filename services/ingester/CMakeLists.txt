cmake_minimum_required(VERSION 3.20)
project(AlphaIngester VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)

# hiredis — C Redis client for XREAD
pkg_check_modules(hiredis REQUIRED hiredis)

# ── Proto generation ──────────────────────────────────────────────────────────
# Generates alpha_tick.pb.h and alpha_tick.pb.cc into the build directory.
# Include ${CMAKE_CURRENT_BINARY_DIR} to find the generated header.
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/proto")

add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/alpha_tick.pb.cc"
            "${CMAKE_CURRENT_BINARY_DIR}/alpha_tick.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            "--proto_path=${PROTO_DIR}"
            "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
            "${PROTO_DIR}/alpha_tick.proto"
    DEPENDS "${PROTO_DIR}/alpha_tick.proto"
    COMMENT "Generating C++ bindings from alpha_tick.proto"
)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(../../shared/cpp/include)
include_directories(${hiredis_INCLUDE_DIRS})
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})   # for alpha_tick.pb.h

# ── Ingester binary (Redis → SHM bridge) ─────────────────────────────────────
add_executable(alpha-ingester
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/alpha_tick.pb.cc"
)

target_link_libraries(alpha-ingester
    Boost::system
    Threads::Threads
    ${hiredis_LIBRARIES}
    ${Protobuf_LIBRARIES}
)

target_compile_options(alpha-ingester PRIVATE
    -O2
    -Wall
    -Wextra
    -Werror
)

# ── Unit tests ────────────────────────────────────────────────────────────────
enable_testing()
find_package(GTest REQUIRED)

add_executable(alpha-ingester-test
    ../../tests/unit/ingester_normalization_test.cpp
    ../../tests/unit/rate_limiter_test.cpp
    ../../tests/unit/ipc_ringbuffer_test.cpp
    ../../tests/unit/proto_tick_mapper_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/alpha_tick.pb.cc"
)

target_include_directories(alpha-ingester-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src       # ProtoMapper.hpp
    ${CMAKE_CURRENT_BINARY_DIR}           # alpha_tick.pb.h
    ../../shared/cpp/include
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(alpha-ingester-test
    GTest::gtest_main
    Threads::Threads
    Boost::system
    ${Protobuf_LIBRARIES}
)

add_test(NAME IngesterUnitTests COMMAND alpha-ingester-test)
