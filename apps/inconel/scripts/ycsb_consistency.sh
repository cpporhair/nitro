#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

SCENARIO="${1:-all}"
BDF="${INCONEL_YCSB_BDF:-0000:04:00.0}"
SYSTEM_BDF="0000:03:00.0"
CONFIG="$ROOT/apps/inconel/ycsb/config.sample.json"
YCSB="$ROOT/build_real/inconel_ycsb"
SPDK_STATUS="/home/null/work/kv/spdk/scripts/setup.sh"
LIBS="${INCONEL_REAL_NVME_LIBS:-/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib}"
LOG_DIR="${INCONEL_YCSB_CONSISTENCY_LOG_DIR:-/tmp/inconel_ycsb_consistency.$$}"
LOCK_FILE="${INCONEL_YCSB_LOCK_FILE:-/tmp/inconel_ycsb_${BDF//[:.]/_}.lock}"

usage() {
    cat <<EOF
Usage: $0 [all|a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10]

Environment:
  INCONEL_YCSB_BDF                  Scratch BDF, default 0000:04:00.0
  INCONEL_REAL_NVME_LIBS            SPDK/DPDK library path
  INCONEL_YCSB_CONSISTENCY_LOG_DIR  Output log directory
  INCONEL_YCSB_LOCK_FILE            Per-BDF lock file
EOF
}

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

require_scratch_bdf() {
    if [[ "$BDF" == "$SYSTEM_BDF" ]]; then
        fail "refusing known system-disk BDF $SYSTEM_BDF"
    fi
}

acquire_bdf_lock() {
    mkdir -p "$(dirname "$LOCK_FILE")"
    exec 9>"$LOCK_FILE"
    if ! flock -n 9; then
        fail "scratch BDF $BDF is already locked by $LOCK_FILE"
    fi
}

build_targets() {
    cmake --build "$ROOT/build_real" \
        --target inconel_ycsb inconel_real_nvme_compile_check \
        -j2
}

check_device_status() {
    require_scratch_bdf
    local status
    status="$(sudo -n "$SPDK_STATUS" status)"
    echo "$status"
    if ! grep -qE "${BDF}.*vfio-pci" <<<"$status"; then
        fail "$BDF is not bound to vfio-pci"
    fi
    if ! grep -qE "${SYSTEM_BDF}.*Active devices: mount@" <<<"$status"; then
        fail "$SYSTEM_BDF did not appear as the mounted system disk; refusing"
    fi
}

prepare() {
    mkdir -p "$LOG_DIR"
    require_scratch_bdf
    acquire_bdf_lock
    if [[ ! -f "$CONFIG" ]]; then
        fail "missing config sample: $CONFIG"
    fi
    build_targets
    if [[ ! -x "$YCSB" ]]; then
        fail "missing executable: $YCSB"
    fi
    check_device_status >"$LOG_DIR/spdk_status.txt"
}

metric() {
    local log="$1"
    local line="$2"
    local key="$3"
    awk -v line="$line" -v key="$key" '
        $1 == line {
            for (i = 2; i <= NF; ++i) {
                split($i, kv, "=")
                if (kv[1] == key) {
                    print kv[2]
                    exit
                }
            }
        }
    ' "$log"
}

require_metric() {
    local log="$1"
    local line="$2"
    local key="$3"
    local value
    value="$(metric "$log" "$line" "$key")"
    if [[ -z "$value" ]]; then
        fail "$log: missing metric ${line}.${key}"
    fi
    echo "$value"
}

assert_eq() {
    local log="$1"
    local line="$2"
    local key="$3"
    local expected="$4"
    local actual
    actual="$(require_metric "$log" "$line" "$key")"
    if [[ "$actual" != "$expected" ]]; then
        fail "$log: expected ${line}.${key}=$expected, got $actual"
    fi
}

assert_gt_zero() {
    local log="$1"
    local line="$2"
    local key="$3"
    local actual
    actual="$(require_metric "$log" "$line" "$key")"
    if [[ "$actual" == "0" ]]; then
        fail "$log: expected ${line}.${key} > 0"
    fi
}

assert_no_phase_errors() {
    local log="$1"
    local phase
    for phase in load load-flush verify run expect; do
        assert_eq "$log" "$phase" "write_errors" "0"
        assert_eq "$log" "$phase" "read_errors" "0"
    done
}

assert_real_run_ok() {
    local log="$1"
    assert_no_phase_errors "$log"
    assert_eq "$log" maintenance failed 0
}

print_summary() {
    local log="$1"
    grep -E '^(load|load-flush|verify|run|expect|maintenance) ' "$log" || true
}

run_local() {
    local name="$1"
    shift
    local log="$LOG_DIR/${name}.log"
    echo "== $name ==" >&2
    if ! "$@" >"$log" 2>&1; then
        cat "$log"
        fail "$name failed"
    fi
    print_summary "$log" >&2
    echo "$log"
}

run_local_expect_exit() {
    local name="$1"
    local expected="$2"
    shift 2
    local log="$LOG_DIR/${name}.log"
    local rc
    echo "== $name ==" >&2
    set +e
    "$@" >"$log" 2>&1
    rc=$?
    set -e
    if [[ "$rc" != "$expected" ]]; then
        cat "$log"
        fail "$name expected exit $expected, got $rc"
    fi
    echo "$log"
}

run_ycsb() {
    local name="$1"
    shift
    run_local "$name" \
        sudo -n env XDG_RUNTIME_DIR=/tmp LD_LIBRARY_PATH="$LIBS" \
        timeout 300s "$YCSB" \
        --config "$CONFIG" \
        --pci "$BDF" \
        --no-print-config \
        "$@"
}

run_a0() {
    local log
    log="$(run_local "a0_dry_run" \
        "$YCSB" --config "$CONFIG" --dry-run --no-print-config \
        --dump-config --pci "$BDF")"
    grep -q "\"pci_addr\": \"$BDF\"" "$log" ||
        fail "$log: dry-run did not use scratch BDF $BDF"

    log="$(run_local_expect_exit "a0_reject_system_bdf" 2 \
        "$YCSB" --config "$CONFIG" --dry-run --no-print-config \
        --pci "$SYSTEM_BDF")"
    grep -q "refusing known system-disk BDF" "$log" ||
        fail "$log: missing system-disk rejection"
}

run_a1() {
    local log
    log="$(run_ycsb "a1_force_load_verify" \
        --force-format --workload load --records 1000 --operations 1000 \
        --verify-samples 64)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 64
    assert_eq "$log" verify read_miss 0
}

run_a2() {
    local log
    log="$(run_ycsb "a2_explicit_flush_after_load" \
        --force-format --workload load --records 1000 --operations 1000 \
        --verify-samples 64 --flush-after-load)"
    assert_real_run_ok "$log"
    assert_eq "$log" load-flush ops 1
    assert_eq "$log" load-flush batches 1
    assert_eq "$log" verify read_found 64
}

run_a3() {
    local log
    log="$(run_ycsb "a3_restart_after_explicit_flush" \
        --workload c --records 1000 --operations 1000 --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
}

run_a4() {
    local log
    log="$(run_ycsb "a4_1_wal_only_load" \
        --force-format --workload load --records 1000 --operations 1000 \
        --verify-samples 64)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 64

    log="$(run_ycsb "a4_2_recover_wal_read" \
        --workload c --records 1000 --operations 1000 --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0

    log="$(run_ycsb "a4_3_clean_restart_read" \
        --workload c --records 1000 --operations 1000 --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
}

run_a5() {
    local log
    log="$(run_ycsb "a5_auto_flush_continuous_writes" \
        --force-format --workload load-a --records 10000 --operations 10000 \
        --value-size 256 --verify-samples 64 \
        --maintenance-seal-active-bytes 65536 \
        --maintenance-total-memtable-bytes 262144 \
        --maintenance-wal-seal-percent 5 \
        --maintenance-max-sealed-gens-per-front 1)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 64
    assert_gt_zero "$log" maintenance seal
    assert_gt_zero "$log" maintenance flush
    assert_gt_zero "$log" maintenance non_noop_flush
}

run_a6() {
    local log
    log="$(run_ycsb "a6_restart_after_auto_flush" \
        --workload c --records 10000 --operations 10000 --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 10000
    assert_eq "$log" run read_miss 0
}

run_a7() {
    local log
    log="$(run_ycsb "a7_1_load_for_updates" \
        --force-format --workload load --records 1000 --operations 1000 \
        --verify-samples 64)"
    assert_real_run_ok "$log"

    log="$(run_ycsb "a7_2_update_wal" \
        --workload update --records 1000 --operations 2000 \
        --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run acked_entries 2000

    log="$(run_ycsb "a7_3_verify_update_winners" \
        --workload c --records 1000 --operations 2000 \
        --verify-existing-updates --verify-samples 128)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 128
    assert_eq "$log" run read_found 2000
    assert_eq "$log" run read_miss 0
}

run_a8() {
    local log
    log="$(run_ycsb "a8_1_load_for_deletes" \
        --force-format --workload load --records 1000 --operations 1000 \
        --verify-samples 64)"
    assert_real_run_ok "$log"

    log="$(run_ycsb "a8_2_delete_wal" \
        --workload del --records 1000 --operations 1000 \
        --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run acked_entries 1000

    log="$(run_ycsb "a8_3_verify_tombstones" \
        --workload c --records 1000 --operations 1000 \
        --verify-existing-deletes --verify-samples 64)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 0
    assert_eq "$log" verify read_miss 64
    assert_eq "$log" run read_found 0
    assert_eq "$log" run read_miss 1000
}

run_a9_continuation() {
    local log
    log="$(run_ycsb "a9_1_put_after_tombstone_recovery" \
        --workload load --records 1000 --operations 1000 \
        --verify-samples 64)"
    assert_real_run_ok "$log"
    assert_eq "$log" verify read_found 64
    assert_eq "$log" verify read_miss 0

    log="$(run_ycsb "a9_2_restart_after_put" \
        --workload c --records 1000 --operations 1000 --verify-samples 0)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
}

run_a9() {
    run_a8
    run_a9_continuation
}

run_a10() {
    local log
    local expect_file="$LOG_DIR/a10_expected_state_a.json"
    log="$(run_ycsb "a10_1_expected_load_a" \
        --force-format --workload load-a --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 \
        --expect-all --write-expect-file "$expect_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" expect read_errors 0
    assert_eq "$log" expect read_miss 0
    assert_gt_zero "$log" expect read_found
    [[ -s "$expect_file" ]] ||
        fail "$expect_file: expected-state file was not written"

    log="$(run_ycsb "a10_2_expected_restart_verify" \
        --workload c --records 1000 --operations 1000 \
        --expect-file "$expect_file" --expect-all)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0

    expect_file="$LOG_DIR/a10_expected_state_b.json"
    log="$(run_ycsb "a10_3_expected_load_b" \
        --force-format --workload load-b --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 \
        --expect-all --write-expect-file "$expect_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" expect read_errors 0
    assert_eq "$log" expect read_miss 0
    assert_gt_zero "$log" expect read_found
    [[ -s "$expect_file" ]] ||
        fail "$expect_file: expected-state file was not written"

    log="$(run_ycsb "a10_4_expected_b_restart_verify" \
        --workload c --records 1000 --operations 1000 \
        --expect-file "$expect_file" --expect-all)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
}

run_all() {
    run_a0
    run_a1
    run_a2
    run_a3
    run_a4
    run_a5
    run_a6
    run_a7
    run_a8
    run_a9_continuation
    run_a10
}

case "$SCENARIO" in
    -h|--help)
        usage
        exit 0
        ;;
    all|a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10)
        prepare
        "run_${SCENARIO}"
        echo "PASS: $SCENARIO logs in $LOG_DIR"
        ;;
    *)
        usage
        fail "unknown scenario '$SCENARIO'"
        ;;
esac
