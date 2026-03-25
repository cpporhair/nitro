#!/usr/bin/env bash
set -euo pipefail

# Sider benchmark script — parameters hardcoded from benchmark.md.
# Usage: sudo ./apps/sider/scripts/bench.sh <scenario> <cores>
#   scenario: hot | cold | set | backpressure
#   cores:    1 | 2 | 4
#
# Examples:
#   sudo ./apps/sider/scripts/bench.sh hot 1
#   sudo ./apps/sider/scripts/bench.sh cold 4
#   sudo ./apps/sider/scripts/bench.sh set 1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SIDER="$ROOT/build/sider"
SB="$ROOT/build/sider_bench"
PORT=6379

# ── Fixed parameters (from benchmark.md §3.2) ──
D=256
R_HOT=1800000
R_COLD=5400000
N_SET_HOT=18000000    # 10x -r for coverage
N_SET_COLD=54000000   # 10x -r for coverage
NVME="0000:02:00.0,0000:04:00.0,0000:06:00.0"
NVME_PCI="0000:02:00.0 0000:04:00.0 0000:06:00.0"
MEMORY=512M

# ── Per-core config (from benchmark.md §3.3, §3.4) ──
declare -A CORES_LIST=( [1]="0" [2]="0,2" [4]="0,2,4,6" )
declare -A BENCH_C=( [1]=50 [2]=100 [4]=200 )
declare -A BENCH_THREADS=( [1]="" [2]="--threads 4" [4]="--threads 8" )
declare -A BENCH_TASKSET=( [1]="" [2]="taskset -c 8,9,10,11" [4]="taskset -c 8,9,10,11,12,13,14,15" )
declare -A N_P32_GET=( [1]=3000000 [2]=4000000 [4]=8000000 )
declare -A N_P1_GET=( [1]=500000 [2]=1000000 [4]=2000000 )

# ── Parse args ──
SCENARIO="${1:-}"
NCORES="${2:-}"

if [[ -z "$SCENARIO" || -z "$NCORES" ]]; then
    echo "Usage: sudo $0 <hot|cold|set|backpressure> <1|2|4>"
    exit 1
fi

if [[ -z "${CORES_LIST[$NCORES]+x}" ]]; then
    echo "Error: cores must be 1, 2, or 4"
    exit 1
fi

C="${BENCH_C[$NCORES]}"
THREADS="${BENCH_THREADS[$NCORES]}"
TASKSET="${BENCH_TASKSET[$NCORES]}"
CORES="${CORES_LIST[$NCORES]}"

sb() {
    # Run sider_bench with taskset if needed.
    if [[ -n "$TASKSET" ]]; then
        $TASKSET "$SB" -h 127.0.0.1 -p $PORT "$@"
    else
        "$SB" -h 127.0.0.1 -p $PORT "$@"
    fi
}

check_env() {
    local gov
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    if [[ "$gov" != "performance" ]]; then
        echo "WARNING: CPU governor is '$gov', should be 'performance'"
        echo "  Run: echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
        exit 1
    fi
    local freq
    freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
    echo "CPU governor: $gov, freq: ${freq}KHz"

    if [[ ! -x "$SIDER" ]]; then
        echo "Error: $SIDER not found. Run: cmake --build build -j\$(nproc)"
        exit 1
    fi
    if [[ ! -x "$SB" ]]; then
        echo "Error: $SB not found. Run: cmake --build build --target sider_bench -j\$(nproc)"
        exit 1
    fi
}

cleanup() {
    pkill -f "sider --port $PORT" 2>/dev/null || true
    sleep 1
}

wait_ready() {
    local tries=0
    while ! redis-cli -p $PORT PING 2>/dev/null | grep -q PONG; do
        sleep 1
        tries=$((tries + 1))
        if [[ $tries -ge 30 ]]; then
            echo "Error: sider not ready after 30s"
            exit 1
        fi
    done
}

print_header() {
    echo "========================================"
    echo "  Sider Benchmark: $SCENARIO ${NCORES}C"
    echo "  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "========================================"
    echo "Server:  --memory $MEMORY --cores $CORES"
    echo "Bench:   -c $C $THREADS -d $D"
    echo "CPU:     $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)KHz"
    echo "========================================"
}

# ── Scenarios ──

run_hot() {
    local R=$R_HOT
    cleanup
    "$SIDER" --port $PORT --memory $MEMORY --cores $CORES &
    wait_ready
    print_header

    echo ""
    echo "--- SET fill (-r $R -n $N_SET_HOT -P 32) ---"
    sb -t set -c $C $THREADS -n $N_SET_HOT -P 32 -q -d $D -r $R 2>&1 | tail -1

    echo ""
    echo "--- P32 GET (-r $R -n ${N_P32_GET[$NCORES]}) ---"
    sb -t get -c $C $THREADS -n ${N_P32_GET[$NCORES]} -P 32 -q -d $D -r $R 2>&1 | grep "per second"

    echo ""
    echo "--- P1 GET (-r $R -n ${N_P1_GET[$NCORES]}) ---"
    sb -t get -c $C $THREADS -n ${N_P1_GET[$NCORES]} -P 1 -q -d $D -r $R 2>&1 | grep "per second"

    echo ""
    echo "CPU after: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)KHz"
    cleanup
}

run_cold() {
    local R=$R_COLD
    cleanup

    # Bind NVMe
    HUGEMEM=8192 PCI_ALLOWED="$NVME_PCI" spdk-setup config 2>&1 | grep -E "vfio|Already"

    "$SIDER" --port $PORT --memory $MEMORY --cores $CORES --nvme $NVME &
    wait_ready
    print_header

    echo ""
    echo "--- SET fill (-r $R -n $N_SET_COLD -P 32) ---"
    sb -t set -c $C $THREADS -n $N_SET_COLD -P 32 -q -d $D -r $R 2>&1 | tail -1

    # Verify
    local vlen
    vlen=$(redis-cli -p $PORT GET "key:000000500000" 2>/dev/null | wc -c)
    echo "Verify key:000000500000 = ${vlen} bytes"
    if [[ "$vlen" -lt 10 ]]; then
        echo "WARNING: verification failed, GET returned $vlen bytes (expected ~257)"
    fi

    echo "Waiting 10s for eviction to stabilize..."
    sleep 10

    echo ""
    echo "--- P32 GET (-r $R -n ${N_P32_GET[$NCORES]}) ---"
    sb -t get -c $C $THREADS -n ${N_P32_GET[$NCORES]} -P 32 -q -d $D -r $R 2>&1 | grep "per second"

    echo ""
    echo "CPU mid: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)KHz"

    echo ""
    echo "--- P1 GET (-r $R -n ${N_P1_GET[$NCORES]}) ---"
    sb -t get -c $C $THREADS -n ${N_P1_GET[$NCORES]} -P 1 -q -d $D -r $R 2>&1 | grep "per second"

    echo ""
    echo "CPU after: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)KHz"
    cleanup

    # Unbind NVMe
    PCI_ALLOWED="$NVME_PCI" spdk-setup reset 2>&1 | grep "vfio"
}

run_set() {
    local R=$R_HOT
    cleanup
    "$SIDER" --port $PORT --memory $MEMORY --cores $CORES &
    wait_ready
    print_header

    echo ""
    echo "--- P32 SET (-r $R -n ${N_P32_GET[$NCORES]}) ---"
    sb -t set -c $C $THREADS -n ${N_P32_GET[$NCORES]} -P 32 -q -d $D -r $R 2>&1 | grep "per second"

    echo ""
    echo "CPU after: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)KHz"
    cleanup
}

run_backpressure() {
    cleanup

    # Bind NVMe
    HUGEMEM=8192 PCI_ALLOWED="$NVME_PCI" spdk-setup config 2>&1 | grep -E "vfio|Already"

    "$SIDER" --port $PORT --memory 10M --cores 0 --nvme $NVME &
    wait_ready
    print_header

    echo ""
    echo "--- Backpressure SET (-c 1 -n 100000 -P 1 -r 100000) ---"
    sb -t set -c 1 -n 100000 -P 1 -q -d $D -r 100000 2>&1 | tail -1

    echo ""
    echo "Manual verify:"
    redis-cli -p $PORT SET testkey testval 2>&1
    cleanup

    PCI_ALLOWED="$NVME_PCI" spdk-setup reset 2>&1 | grep "vfio"
}

# ── Main ──
check_env

case "$SCENARIO" in
    hot)            run_hot ;;
    cold)           run_cold ;;
    set)            run_set ;;
    backpressure)   run_backpressure ;;
    *)
        echo "Error: unknown scenario '$SCENARIO'"
        echo "Valid: hot | cold | set | backpressure"
        exit 1
        ;;
esac

echo ""
echo "Done."
