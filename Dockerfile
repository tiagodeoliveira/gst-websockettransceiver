# Build and test environment for gst-websockettransceiver
FROM debian:bookworm-slim

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build tools
    meson \
    ninja-build \
    pkg-config \
    gcc \
    # GStreamer development libraries
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    # libsoup for WebSocket
    libsoup-3.0-dev \
    # Python for integration tests
    python3 \
    python3-pip \
    python3-websockets \
    # Utilities
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /src

# Copy source code
COPY . .

# Build
RUN meson setup build && meson compile -C build

# Default command: run tests
CMD ["meson", "test", "-C", "build", "-v"]
