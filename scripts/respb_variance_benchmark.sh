#!/bin/bash

# RESPB vs RESP3 Variance Benchmark
# Runs multiple trials GROUPED BY SIZE to reduce variance

PORT=6399
CLIENTS=128
PIPELINE=128
REQUESTS=1000000
NUM_TRIALS=${1:-5}

SIZES="256 384 512 640 768 896 1024 1152 1280 1408 1536 1664 1792 1920 2048"
RESULTS_FILE="/tmp/respb_bench_results.csv"

echo "=============================================="
echo "RESPB vs RESP3 Variance Benchmark"
echo "=============================================="
echo "Configuration:"
echo "  Trials per size: $NUM_TRIALS"
echo "  Clients: $CLIENTS"
echo "  Pipeline: $PIPELINE"
echo "  Requests per test: $REQUESTS"
echo "=============================================="
echo ""

# Start server
echo "Starting Valkey server..."
pkill -9 valkey-server 2>/dev/null || true
sleep 1
./src/valkey-server --daemonize yes --logfile /tmp/valkey-bench.log --port $PORT --save ""
sleep 2

if ! pgrep -f "valkey-server.*$PORT" > /dev/null; then
    echo "ERROR: Failed to start server"
    exit 1
fi
echo "Server started on port $PORT"
echo ""

# Clear results file
echo "size,trial,resp3_rps,respb_rps,ratio" > $RESULTS_FILE

# Run trials GROUPED BY SIZE (reduces variance)
for SIZE in $SIZES; do
    echo "=== Size $SIZE bytes: $NUM_TRIALS trials ==="

    for TRIAL in $(seq 1 $NUM_TRIALS); do
        # Run RESP3 and RESPB back-to-back for same size (reduces system state variance)
        RESP3=$(./src/valkey-benchmark -p $PORT -3 -c $CLIENTS -P $PIPELINE -n $REQUESTS -t set -d $SIZE -r 100000 --csv 2>/dev/null | grep -i "set")
        RESP3_RPS=$(echo "$RESP3" | cut -d',' -f2 | tr -d '"')

        RESPB=$(./src/valkey-benchmark -p $PORT -4 -c $CLIENTS -P $PIPELINE -n $REQUESTS -t set -d $SIZE -r 100000 --csv 2>/dev/null | grep -i "set")
        RESPB_RPS=$(echo "$RESPB" | cut -d',' -f2 | tr -d '"')

        RATIO=$(echo "scale=3; $RESPB_RPS / $RESP3_RPS" | bc)

        # Save to CSV
        echo "$SIZE,$TRIAL,$RESP3_RPS,$RESPB_RPS,$RATIO" >> $RESULTS_FILE

        printf "  Trial %d: RESP3=%12s  RESPB=%12s  Ratio=%s\n" "$TRIAL" "$RESP3_RPS" "$RESPB_RPS" "$RATIO"
    done

    # Calculate and show average for this size
    RATIOS=$(grep "^$SIZE," $RESULTS_FILE | cut -d',' -f5)
    SUM=0
    COUNT=0
    for R in $RATIOS; do
        SUM=$(echo "$SUM + $R" | bc)
        COUNT=$((COUNT + 1))
    done
    AVG=$(echo "scale=3; $SUM / $COUNT" | bc)
    echo "  Average Ratio: $AVG"
    echo ""
done

# Cleanup server
echo "Stopping server..."
pkill -f "valkey-server.*$PORT" 2>/dev/null || true
echo ""

echo "=============================================="
echo "AGGREGATED RESULTS"
echo "=============================================="
echo ""

printf "%-6s | %-12s | %-12s | %-9s | %-9s | %-9s | %-8s\n" \
    "Size" "Avg RESP3" "Avg RESPB" "Avg Ratio" "Min Ratio" "Max Ratio" "Winner"
echo "-------|--------------|--------------|-----------|-----------|-----------|--------"

RESPB_WINS=0
RESP3_WINS=0
TIES=0

for SIZE in $SIZES; do
    # Get all values for this size
    RESP3_VALS=$(grep "^$SIZE," $RESULTS_FILE | cut -d',' -f3)
    RESPB_VALS=$(grep "^$SIZE," $RESULTS_FILE | cut -d',' -f4)
    RATIOS=$(grep "^$SIZE," $RESULTS_FILE | cut -d',' -f5)

    # Calculate averages
    RESP3_SUM=0
    RESPB_SUM=0
    COUNT=0
    for VAL in $RESP3_VALS; do
        RESP3_SUM=$(echo "$RESP3_SUM + $VAL" | bc)
        COUNT=$((COUNT + 1))
    done
    for VAL in $RESPB_VALS; do
        RESPB_SUM=$(echo "$RESPB_SUM + $VAL" | bc)
    done

    RESP3_AVG=$(echo "scale=2; $RESP3_SUM / $COUNT" | bc)
    RESPB_AVG=$(echo "scale=2; $RESPB_SUM / $COUNT" | bc)
    AVG_RATIO=$(echo "scale=3; $RESPB_AVG / $RESP3_AVG" | bc)

    # Find min and max ratio
    MIN_RATIO=$(echo "$RATIOS" | tr ' ' '\n' | sort -n | head -1)
    MAX_RATIO=$(echo "$RATIOS" | tr ' ' '\n' | sort -n | tail -1)

    # Determine winner (using 2% threshold)
    if [ "$(echo "$AVG_RATIO > 1.02" | bc -l)" -eq 1 ]; then
        WINNER="RESPB"
        RESPB_WINS=$((RESPB_WINS + 1))
    elif [ "$(echo "$AVG_RATIO < 0.98" | bc -l)" -eq 1 ]; then
        WINNER="RESP3"
        RESP3_WINS=$((RESP3_WINS + 1))
    else
        WINNER="TIE"
        TIES=$((TIES + 1))
    fi

    printf "%-6s | %12.0f | %12.0f | %9s | %9s | %9s | %-8s\n" \
        "$SIZE" "$RESP3_AVG" "$RESPB_AVG" "$AVG_RATIO" "$MIN_RATIO" "$MAX_RATIO" "$WINNER"
done

echo ""
echo "=============================================="
echo "SUMMARY"
echo "=============================================="
echo ""
echo "Final Score:"
echo "  RESPB wins: $RESPB_WINS / 15 sizes"
echo "  RESP3 wins: $RESP3_WINS / 15 sizes"
echo "  Ties:       $TIES / 15 sizes"
echo ""
echo "Results saved to: $RESULTS_FILE"
echo "Benchmark complete!"
