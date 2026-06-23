#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

SCENARIO="${1:-all}"
BDF="${INCONEL_YCSB_BDF:-0000:04:00.0}"
SYSTEM_BDF="0000:03:00.0"
CONFIG="$ROOT/apps/inconel/ycsb/config.sample.json"
YCSB="$ROOT/build_real/inconel_ycsb"
CONCURRENCY="$ROOT/build_real/inconel_test_ycsb_concurrency_checker_e2e"
SPDK_STATUS="/home/null/work/kv/spdk/scripts/setup.sh"
LIBS="${INCONEL_REAL_NVME_LIBS:-/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib}"
LOG_DIR="${INCONEL_YCSB_CONSISTENCY_LOG_DIR:-/tmp/inconel_ycsb_consistency.$$}"
LOCK_FILE="/tmp/inconel_ycsb_${BDF//[:.]/_}.lock"

usage() {
    cat <<EOF
Usage: $0 [all|a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10|c1|c2|c3|c4|c5|c6|c7|c8]

Environment:
  INCONEL_YCSB_BDF                  Scratch BDF, default 0000:04:00.0
  INCONEL_REAL_NVME_LIBS            SPDK/DPDK library path
  INCONEL_YCSB_CONSISTENCY_LOG_DIR  Output log directory
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
        --target \
        inconel_ycsb \
        inconel_real_nvme_compile_check \
        inconel_test_ycsb_concurrency_checker_e2e \
        -j2
}

check_device_status() {
    require_scratch_bdf
    local status
    status="$(sudo -n "$SPDK_STATUS" status)"
    echo "$status"
    if ! awk -v bdf="$BDF" '
        index($0, bdf) && index($0, "vfio-pci") { found = 1 }
        END { exit found ? 0 : 1 }
    ' <<<"$status"; then
        fail "$BDF is not bound to vfio-pci"
    fi
    if ! awk -v bdf="$SYSTEM_BDF" '
        index($0, bdf) && index($0, "Active devices: mount@") { found = 1 }
        END { exit found ? 0 : 1 }
    ' <<<"$status"; then
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
    if [[ ! -x "$CONCURRENCY" ]]; then
        fail "missing executable: $CONCURRENCY"
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
    if ! [[ "$actual" =~ ^[0-9]+$ ]]; then
        fail "$log: expected numeric ${line}.${key}, got $actual"
    fi
    if (( actual == 0 )); then
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
    grep -E '^(load|load-flush|verify|run|expect|maintenance|checker|checker_barrier|checker_frontier_barrier|checker_frontier_window|checker_maintenance) ' "$log" || true
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

run_concurrency_checker() {
    local name="$1"
    local scenario="$2"
    run_local "$name" \
        sudo -n env XDG_RUNTIME_DIR=/tmp LD_LIBRARY_PATH="$LIBS" \
        INCONEL_ALLOWED_SCRATCH_BDF="$BDF" \
        timeout 300s "$CONCURRENCY" \
        --pci-addr "$BDF" \
        --scenario "$scenario"
}

assert_checker_ok() {
    local log="$1"
    grep -q "^all passed$" "$log" ||
        fail "$log: checker did not report all passed"
    assert_eq "$log" checker writes 4160
    assert_eq "$log" checker hot_keys 64
    assert_gt_zero "$log" checker reads
}

assert_checker_maintenance_ok() {
    local log="$1"
    assert_eq "$log" checker_maintenance failed 0
    assert_gt_zero "$log" checker_maintenance seal
}

assert_checker_flush_ok() {
    local log="$1"
    assert_checker_maintenance_ok "$log"
    assert_gt_zero "$log" checker_maintenance flush
    assert_gt_zero "$log" checker_maintenance non_noop_flush
}

assert_checker_batch_barrier_ok() {
    local log="$1"
    assert_eq "$log" checker_barrier reads 4096
}

assert_checker_frontier_barrier_ok() {
    local log="$1"
    assert_eq "$log" checker_frontier_barrier reads 64
    assert_eq "$log" checker_frontier_barrier generation 2
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
    assert_eq "$log" run acked_entries 509
    assert_eq "$log" expect read_errors 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
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
    assert_eq "$log" run acked_entries 56
    assert_eq "$log" expect read_errors 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
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

run_c1() {
    local log
    log="$(run_concurrency_checker "c1_put_read_race" "c1")"
    assert_checker_ok "$log"
}

run_c2() {
    local log
    log="$(run_concurrency_checker "c2_put_read_auto_seal_race" "c2")"
    assert_checker_ok "$log"
    assert_checker_maintenance_ok "$log"
}

run_c3() {
    local log
    log="$(run_concurrency_checker "c3_put_read_auto_flush_race" "c3")"
    assert_checker_ok "$log"
    assert_checker_flush_ok "$log"
}

run_c4() {
    local log
    log="$(run_concurrency_checker "c4_delete_read_put_race" "c4")"
    assert_checker_ok "$log"
}

run_c5() {
    local log
    log="$(run_concurrency_checker "c5_batch_ack_barrier" "c5")"
    assert_checker_ok "$log"
    assert_checker_batch_barrier_ok "$log"
    assert_checker_flush_ok "$log"
}

run_c6() {
    local log
    log="$(run_concurrency_checker "c6_frontier_switch_barrier" "c6")"
    grep -q "^all passed$" "$log" ||
        fail "$log: checker did not report all passed"
    assert_eq "$log" checker writes 128
    assert_eq "$log" checker hot_keys 64
    assert_gt_zero "$log" checker reads
    assert_eq "$log" checker_barrier reads 64
    assert_checker_frontier_barrier_ok "$log"
    assert_gt_zero "$log" checker_frontier_window reads
    assert_checker_flush_ok "$log"
}

run_c7() {
    local log
    local base_file="$LOG_DIR/c7_base_expected.json"
    local delta_file="$LOG_DIR/c7_delta_expected.json"
    local after_file="$LOG_DIR/c7_after_recovery_expected.json"

    log="$(run_ycsb "c7_1_load_flush_base" \
        --force-format --workload load --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 --flush-after-load \
        --expect-all --write-expect-file "$base_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" load-flush ops 1
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
    [[ -s "$base_file" ]] ||
        fail "$base_file: expected-state file was not written"

    log="$(run_ycsb "c7_2_update_wal_delta" \
        --workload update --records 1000 --operations 2000 \
        --inflight 1 --verify-samples 0 \
        --expect-file "$base_file" --expect-all \
        --write-expect-file "$delta_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" run acked_entries 2000
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
    [[ -s "$delta_file" ]] ||
        fail "$delta_file: expected-state file was not written"

    log="$(run_ycsb "c7_3_recover_verify_delta" \
        --workload c --records 1000 --operations 1000 \
        --expect-file "$delta_file" --expect-all)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0

    log="$(run_ycsb "c7_4_put_after_recovery" \
        --workload load --records 1000 --operations 1000 \
        --seed 2 --inflight 1 --verify-samples 0 \
        --expect-all --write-expect-file "$after_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" load acked_entries 1000
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
    [[ -s "$after_file" ]] ||
        fail "$after_file: expected-state file was not written"

    log="$(run_ycsb "c7_5_restart_verify_continuation" \
        --workload c --records 1000 --operations 1000 \
        --seed 2 --expect-file "$after_file" --expect-all)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 1000
    assert_eq "$log" run read_miss 0
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
}

run_c8() {
    local log
    local base_file="$LOG_DIR/c8_base_expected.json"
    local tomb_file="$LOG_DIR/c8_tombstone_expected.json"
    local after_file="$LOG_DIR/c8_after_put_expected.json"

    log="$(run_ycsb "c8_1_load_flush_for_tombstones" \
        --force-format --workload load --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 --flush-after-load \
        --expect-all --write-expect-file "$base_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" load-flush ops 1
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
    [[ -s "$base_file" ]] ||
        fail "$base_file: expected-state file was not written"

    log="$(run_ycsb "c8_2_delete_wal_delta" \
        --workload del --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 \
        --expect-file "$base_file" --expect-all \
        --write-expect-file "$tomb_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" run acked_entries 1000
    assert_eq "$log" expect read_found 0
    assert_eq "$log" expect read_miss 1000
    [[ -s "$tomb_file" ]] ||
        fail "$tomb_file: expected-state file was not written"

    log="$(run_ycsb "c8_3_recover_verify_tombstones" \
        --workload c --records 1000 --operations 1000 \
        --expect-file "$tomb_file" --expect-all)"
    assert_real_run_ok "$log"
    assert_eq "$log" run read_found 0
    assert_eq "$log" run read_miss 1000
    assert_eq "$log" expect read_found 0
    assert_eq "$log" expect read_miss 1000

    log="$(run_ycsb "c8_4_put_after_tombstone_recovery" \
        --workload load --records 1000 --operations 1000 \
        --inflight 1 --verify-samples 0 \
        --expect-file "$tomb_file" --expect-all \
        --write-expect-file "$after_file")"
    assert_real_run_ok "$log"
    assert_eq "$log" load acked_entries 1000
    assert_eq "$log" expect read_found 1000
    assert_eq "$log" expect read_miss 0
    [[ -s "$after_file" ]] ||
        fail "$after_file: expected-state file was not written"

    log="$(run_ycsb "c8_5_restart_verify_puts" \
        --workload c --records 1000 --operations 1000 \
        --expect-file "$after_file" --expect-all)"
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
    run_c1
    run_c2
    run_c3
    run_c4
    run_c5
    run_c6
    run_c7
    run_c8
}

case "$SCENARIO" in
    -h|--help)
        usage
        exit 0
        ;;
    all|a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10|c1|c2|c3|c4|c5|c6|c7|c8)
        prepare
        "run_${SCENARIO}"
        echo "PASS: $SCENARIO logs in $LOG_DIR"
        ;;
    *)
        usage
        fail "unknown scenario '$SCENARIO'"
        ;;
esac
