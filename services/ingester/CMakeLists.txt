cmake_minimum_required(VERSION 3.20)
project(AlphaIngester VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
# In Debian, libpqxx doesn't always have a strict CMake config finder out of the box, we can use pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)
# nlohmann_json is often a header-only library, we'll assume it's in the include paths

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
    ${PROTO_SRCS}
)

# Executable
add_executable(alpha-ingester ${SOURCES})

# Testing (as per project rules)
enable_testing()
find_package(GTest REQUIRED)
add_executable(alpha-ingester-test 
    ../../tests/unit/ingester_normalization_test.cpp
    ../../tests/unit/options_decode_test.cpp
    ../../tests/unit/rate_limiter_test.cpp
    ../../tests/unit/ipc_ringbuffer_test.cpp
    src/UpstoxIngester.cpp 
    ${PROTO_SRCS}
)
target_link_libraries(alpha-ingester-test
    GTest::gtest_main
    Threads::Threads
    ${Protobuf_LIBRARIES}
    ${pqxx_LIBRARIES}
)
add_test(NAME IngesterUnitTests COMMAND alpha-ingester-test)

# Linking
target_link_libraries(alpha-ingester
    Boost::system
    Threads::Threads
    ${Protobuf_LIBRARIES}
    ${pqxx_LIBRARIES}
)

# Optimization Flags for HFT Development
target_compile_options(alpha-ingester PRIVATE
    -O2
    -Wall
    -Wextra
    -Werror
)
