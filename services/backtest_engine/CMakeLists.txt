cmake_minimum_required(VERSION 3.20)
project(AlphaBacktestEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(pqxx REQUIRED libpqxx)

# ── Include paths ─────────────────────────────────────────────────────────
include_directories(include)
include_directories(../../shared/cpp/include)
include_directories(${pqxx_INCLUDE_DIRS})

# ── Sources ───────────────────────────────────────────────────────────────
set(BACKTEST_SOURCES
    src/main.cpp
    src/TickLoader.cpp
    src/SimRunner.cpp
    src/ResultAggregator.cpp
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(alpha-backtest ${BACKTEST_SOURCES})

target_link_libraries(alpha-backtest
    Threads::Threads
    ${pqxx_LIBRARIES}
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
)

# ── Optional: link with jemalloc for better allocation performance ─────────
# find_library(JEMALLOC_LIB jemalloc)
# if(JEMALLOC_LIB)
#     target_link_libraries(alpha-backtest ${JEMALLOC_LIB})
# endif()
