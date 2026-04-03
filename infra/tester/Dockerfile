# --- Unified Test Environment ---
FROM debian:bookworm-slim

# Install runtime dependencies for C++ and Python
RUN apt-get update && apt-get install -y \
    libboost-system1.81.0 \
    libprotobuf32 \
    libpqxx-dev \
    libssl3 \
    ca-certificates \
    tzdata \
    python3 \
    python3-pip \
    python3-venv \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy built C++ tests from the existing service images (assuming they are built already)
# To keep this simple and avoid complex multi-image COPY, we'll mount them or assume this Dockerfile 
# is used in a context where those binaries are available.
# Actually, for a pure Docker-based setup, we'll build it here:

# Build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-dev \
    libgtest-dev \
    libprotobuf-dev \
    protobuf-compiler \
    nlohmann-json3-dev \
    libpqxx-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy the entire Alpha folder structure
COPY . .

# Build C++ units
WORKDIR /app/services/ingester
RUN mkdir -p build && cd build && cmake .. && make -j$(nproc) alpha-ingester-test

# Install Python dependencies from the worker_service
WORKDIR /app
RUN pip3 install --no-cache-dir --break-system-packages -r services/worker_service/requirements.txt

# Copy test runner script
COPY tests/run_tests.sh /app/run_tests.sh
RUN chmod +x /app/run_tests.sh

ENTRYPOINT ["/app/run_tests.sh"]
