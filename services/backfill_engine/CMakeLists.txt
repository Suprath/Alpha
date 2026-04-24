cmake_minimum_required(VERSION 3.20)
project(AlphaBackfillEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)
find_package(nlohmann_json REQUIRED)

# ── Include paths ─────────────────────────────────────────────────────────
include_directories(include)
include_directories(../../shared/cpp/include)
include_directories(${pqxx_INCLUDE_DIRS})

# ── Sources ───────────────────────────────────────────────────────────────
set(BACKFILL_SOURCES
    src/main.cpp
    src/QuestDBILPWriter.cpp
    src/TickBuffer.cpp
    src/UpstoxHistoricalFeed.cpp
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(alpha-backfill ${BACKFILL_SOURCES})

target_link_libraries(alpha-backfill
    Boost::system
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${pqxx_LIBRARIES}
    nlohmann_json::nlohmann_json
)

# ── Optimization flags (HFT) ──────────────────────────────────────────────
target_compile_options(alpha-backfill PRIVATE
    -O2
    -march=native
    -Wall
    -Wextra
    -Werror
)
