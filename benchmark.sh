#!/usr/bin/env bash
set -e

UNDERLYING=/mnt/underlying
MOUNTPOINT=/mnt/fuse_mount
BINARY=./fuse_cache
RESULTS_DIR=./results

mkdir -p "$RESULTS_DIR"

run_bench() {
    local MODE=$1
    local FLAG=$2
    local OUTFILE="$RESULTS_DIR/${MODE}.txt"

    echo "=== Benchmarking: $MODE ==="

    # Mount
    $BINARY "$MOUNTPOINT" --underlying="$UNDERLYING" $FLAG -f &
    FUSE_PID=$!
    sleep 1

    # Sequential write throughput
    echo "--- Sequential write (128MB) ---" >> "$OUTFILE"
    fio --name=seqwrite --directory="$MOUNTPOINT" \
        --ioengine=sync --rw=write --bs=4k \
        --size=128m --numjobs=1 --time_based=0 \
        --output-format=terse >> "$OUTFILE" 2>&1

    # Random write IOPS
    echo "--- Random write IOPS ---" >> "$OUTFILE"
    fio --name=randwrite --directory="$MOUNTPOINT" \
        --ioengine=sync --rw=randwrite --bs=4k \
        --size=64m --numjobs=1 --runtime=10 --time_based \
        --output-format=terse >> "$OUTFILE" 2>&1

    # Sequential read (tests read-from-cache for write-back)
    echo "--- Sequential read after write ---" >> "$OUTFILE"
    fio --name=seqread --directory="$MOUNTPOINT" \
        --ioengine=sync --rw=read --bs=4k \
        --size=64m --numjobs=1 --time_based=0 \
        --output-format=terse >> "$OUTFILE" 2>&1

    # Unmount
    fusermount3 -u "$MOUNTPOINT" || kill $FUSE_PID
    wait $FUSE_PID 2>/dev/null || true

    echo "Results saved to $OUTFILE"
}

run_bench "writeback"    ""
run_bench "writethrough" "--writethrough"

# Print summary
echo ""
echo "=== Summary ==="
for MODE in writeback writethrough; do
    echo "--- $MODE ---"
    grep -E "(BW=|IOPS=)" "$RESULTS_DIR/${MODE}.txt" | head -6 || true
done