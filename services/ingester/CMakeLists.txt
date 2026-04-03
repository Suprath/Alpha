cmake_minimum_required(VERSION 3.20)
project(AlphaIngester VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)
find_package(OpenSSL REQUIRED)

# Generate Protobuf classes
set(PROTO_FILE ../../shared/proto/market_data_v3.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILE})

# Include directories
include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}) # For generated proto headers
include_directories(../../shared/cpp/include)
include_directories(${pqxx_INCLUDE_DIRS})

# Source files
set(SOURCES
    src/main.cpp
    src/UpstoxIngester.cpp
    src/UpstoxFeed.cpp
    src/UpstoxLiveFeed.cpp
    src/UpstoxOpsFeed.cpp
    src/IngesterFactory.cpp
    ${PROTO_SRCS}
)

# Executable
add_executable(alpha-ingester ${SOURCES})

# Testing
enable_testing()
find_package(GTest REQUIRED)
add_executable(alpha-ingester-test 
    ../../tests/unit/ingester_normalization_test.cpp
    ../../tests/unit/options_decode_test.cpp
    ../../tests/unit/rate_limiter_test.cpp
    ../../tests/unit/ipc_ringbuffer_test.cpp
    ../../tests/unit/upstox_historical_test.cpp
    src/UpstoxIngester.cpp 
    src/UpstoxFeed.cpp
    src/UpstoxLiveFeed.cpp
    src/UpstoxOpsFeed.cpp
    src/IngesterFactory.cpp
    ${PROTO_SRCS}
)
target_link_libraries(alpha-ingester-test
    GTest::gtest_main
    Threads::Threads
    ${Protobuf_LIBRARIES}
    ${pqxx_LIBRARIES}
    OpenSSL::SSL
)
add_test(NAME IngesterUnitTests COMMAND alpha-ingester-test)

# Linking
target_link_libraries(alpha-ingester
    Boost::system
    Threads::Threads
    ${Protobuf_LIBRARIES}
    ${pqxx_LIBRARIES}
    OpenSSL::SSL
)

# Optimization Flags for HFT Development
target_compile_options(alpha-ingester PRIVATE
    -O2
    -Wall
    -Wextra
    -Werror
)
