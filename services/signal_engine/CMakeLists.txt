cmake_minimum_required(VERSION 3.20)
project(alpha-signal-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Includes
include_directories(include)
include_directories(../../shared/cpp/include)

# Boost (Interprocess) Let's find Boost using the system libraries
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Main Binary
add_executable(alpha-signal-engine 
    src/main.cpp
    src/shm_reader/ShmReader.cpp
    src/shm_writer/ShmWriter.cpp
)

target_include_directories(alpha-signal-engine PRIVATE 
    src
    ../../shared/cpp/include
)

target_link_libraries(alpha-signal-engine PRIVATE 
    pthread 
    rt
)
