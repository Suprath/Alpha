cmake_minimum_required(VERSION 3.20)
project(AlphaIngester VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

# Include directories
include_directories(include)
include_directories(/usr/local/include) # Shared headers path or system paths
include_directories(../../shared/cpp/include)

# Source files
set(SOURCES
    src/main.cpp
)

# Executable
add_executable(alpha-ingester ${SOURCES})

# Testing (as per project rules)
enable_testing()
find_package(GTest REQUIRED)
add_executable(alpha-ingester-test ../../tests/unit/ingester_test.cpp)
target_link_libraries(alpha-ingester-test
    GTest::gtest_main
    Threads::Threads
)
add_test(NAME IngesterUnitTests COMMAND alpha-ingester-test)

# Linking
target_link_libraries(alpha-ingester
    Boost::system
    Threads::Threads
)

# Optimization Flags for HFT
target_compile_options(alpha-ingester PRIVATE
    -O3
    -march=native
    -flto
    -Wall
    -Wextra
    -Werror
)
