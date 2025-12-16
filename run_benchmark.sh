#!/bin/bash

# Benchmark Configuration
# Adjust these based on your system
CLIENTS=128          # Number of parallel connections
PIPELINE=128         # Commands per pipeline
REQUESTS=10000000    # Total requests per test
KEYSPACE=100000      # Number of unique keys
DATASIZE=1024        # Value size in bytes

# Display system info
echo ""
echo "System Information:"
echo "  CPU Cores: $(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo "unknown")"
echo "  Memory: $(sysctl -n hw.memsize 2>/dev/null | awk '{print $1/1024/1024/1024 " GB"}' || free -h 2>/dev/null | awk '/^Mem:/ {print $2}' || echo "unknown")"
echo "  OS: $(uname -s) $(uname -r)"
echo ""

# ==============================================================================
# Server Setup with Performance Tunings
# ==============================================================================

echo "=============================================="
echo "Starting Valkey Server"
echo "=============================================="

# Kill any existing servers
pkill -9 -f valkey-server 2>/dev/null
sleep 1

# Start server with performance-optimized settings
./src/valkey-server \
    --daemonize yes \
    --logfile /tmp/valkey.log \
    --save "" \
    --appendonly no \
    --maxmemory 2gb \
    --maxmemory-policy noeviction \
    --activerehashing no \
    --loglevel warning \
    --tcp-backlog 511 \
    --io-threads 4 \
    --io-threads-do-reads yes \
    --disable-thp yes

# Wait for server to start
sleep 2

# Verify server is running
if ! pgrep -f valkey-server > /dev/null; then
    echo "❌ ERROR: Server failed to start"
    echo "Check logs: tail -50 /tmp/valkey.log"
    exit 1
fi

echo "✓ Server started successfully"
echo ""

# ==============================================================================
# Warmup Phase
# ==============================================================================

echo "=============================================="
echo "Warmup Phase (populating cache)"
echo "=============================================="

# Small warmup to populate caches and stabilize JIT
./src/valkey-benchmark -4 -c 50 -P 10 -n 100000 -r $KEYSPACE -t set,get -d $DATASIZE -q > /dev/null 2>&1
echo "✓ Warmup complete"
echo ""

# ==============================================================================
# Main Benchmarks
# ==============================================================================

echo "=============================================="
echo "RESP3 vs RESPB Benchmark Comparison"
echo "=============================================="
echo "Configuration:"
echo "  Clients (-c): $CLIENTS"
echo "  Pipeline (-P): $PIPELINE"
echo "  Requests (-n): $REQUESTS"
echo "  Keyspace (-r): $KEYSPACE"
echo "  Data size (-d): $DATASIZE bytes"
echo ""

# Run RESP3 benchmark
echo "=== RESP3 (-3) Benchmark ==="
./src/valkey-benchmark \
    -3 \
    -c $CLIENTS \
    -P $PIPELINE \
    -n $REQUESTS \
    -r $KEYSPACE \
    -t set,get \
    -d $DATASIZE \
    --csv

echo ""

# Brief pause between tests
sleep 2

# Run RESPB benchmark
echo "=== RESPB (-4) Benchmark ==="
./src/valkey-benchmark \
    -4 \
    -c $CLIENTS \
    -P $PIPELINE \
    -n $REQUESTS \
    -r $KEYSPACE \
    -t set,get \
    -d $DATASIZE \
    --csv

echo ""

# ==============================================================================
# Pipeline Depth Comparison (Optional - comment out if too slow)
# ==============================================================================

echo ""
echo "=============================================="
echo "Pipeline Depth Analysis"
echo "=============================================="
echo "Testing various pipeline depths to find optimal point"
echo ""

for P in 1 10 50 100 200; do
    echo "--- Pipeline Depth: $P ---"

    echo "RESP3 (P=$P):"
    ./src/valkey-benchmark -3 -c 64 -P $P -n 1000000 -r $KEYSPACE -t get -d $DATASIZE -q --csv

    echo "RESPB (P=$P):"
    ./src/valkey-benchmark -4 -c 64 -P $P -n 1000000 -r $KEYSPACE -t get -d $DATASIZE -q --csv

    echo ""
done

# ==============================================================================
# Cleanup
# ==============================================================================

echo "=============================================="
echo "Cleanup"
echo "=============================================="

# Stop server
pkill -f valkey-server
echo "✓ Server stopped"

echo ""
echo "Benchmark complete!"
echo "Server logs available at: /tmp/valkey.log"
