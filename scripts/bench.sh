#!/usr/bin/env bash
# bench.sh: run the benchmarks and keep the results.
#
#   scripts/bench.sh [options] [-- arguments for planck-bench]
#
#   --clang            build with Clang instead of GCC
#   --cpus A,B         pin planck-bench: measuring thread on A, second on B
#   --venue            also start planckd and drive it with planck-loadgen
#   --venue-cpus LIST  pin planckd, e.g. 4-6. Three CPUs for one shard.
#   --loadgen-cpu N    pin planck-loadgen
#   --orders N         orders per load generator run          (200000)
#   --rate N           orders a second for the fixed-rate run  (50000)
#   --name TEXT        a word to add to the result directory's name
#
# Results go to bench-results/<date>-<time>-<host>[-name]/ as CSV, with a
# note of what machine and build they came from. If an earlier run is there,
# the new one is compared with it.
#
# Read docs/benchmarks.md before believing any of the numbers.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compiler="gcc"
cpus=""
venue=0
venue_cpus=""
loadgen_cpu=""
orders=200000
rate=50000
name=""
extra=()

while [ $# -gt 0 ]; do
    case "$1" in
        --clang)        compiler="clang" ;;
        --gcc)          compiler="gcc" ;;
        --cpus)         cpus="$2"; shift ;;
        --venue)        venue=1 ;;
        --venue-cpus)   venue_cpus="$2"; shift ;;
        --loadgen-cpu)  loadgen_cpu="$2"; shift ;;
        --orders)       orders="$2"; shift ;;
        --rate)         rate="$2"; shift ;;
        --name)         name="-$2"; shift ;;
        -h|--help)      sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)             shift; extra=("$@"); break ;;
        *)              echo "bench.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done

# Line the CSV up in columns for reading, where the tool for that exists.
pretty() {
    if command -v column > /dev/null 2>&1; then column -s, -t; else cat; fi
}

"$root/build.sh" build "--${compiler}" --release
bin="$root/build/${compiler}-release"

results="$root/bench-results"
previous="$(ls -1d "$results"/*/ 2>/dev/null | tail -n 1 || true)"
out="$results/$(date +%Y%m%d-%H%M%S)-$(hostname -s)${name}"
mkdir -p "$out"

{
    echo "date:     $(date -Is)"
    echo "host:     $(hostname)"
    echo "kernel:   $(uname -sr)"
    echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
    echo "cpus:     $(nproc)"
    echo "isolated: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo unknown)"
    echo "compiler: $("$(grep -m1 '^CMAKE_CXX_COMPILER:' "$bin/CMakeCache.txt" | cut -d= -f2)" --version | head -n 1)"
    echo "pinning:  bench=${cpus:-none} venue=${venue_cpus:-none} loadgen=${loadgen_cpu:-none}"
    if grep -qi microsoft /proc/version 2>/dev/null; then
        echo "note:     WSL. See W-30 in docs/weaknesses.md before trusting these."
    fi
} > "$out/environment.txt"

bench_arguments=(--csv)
if [ -n "$cpus" ]; then
    bench_arguments+=(--cpu "${cpus%%,*}")
    [ "${cpus#*,}" != "$cpus" ] && bench_arguments+=(--cpu2 "${cpus#*,}")
fi
echo "==> planck-bench"
"$bin/bench/planck-bench" "${bench_arguments[@]}" ${extra[@]+"${extra[@]}"} \
    2> "$out/bench.log" | tee "$out/bench.csv" | pretty

if [ "$venue" -eq 1 ]; then
    base=$((40000 + ($$ % 2000) * 4))
    venue_arguments=(--listen "127.0.0.1:$base" --feed "127.0.0.1:$((base + 1))"
                     --retransmit "127.0.0.1:$((base + 3))" --stats 0)
    [ -n "$venue_cpus" ] && venue_arguments+=(--cpus "$venue_cpus")
    "$bin/apps/planckd" "${venue_arguments[@]}" > "$out/planckd.log" 2>&1 &
    venue_pid=$!
    trap 'kill "$venue_pid" 2>/dev/null; wait 2>/dev/null' EXIT
    for _ in $(seq 1 100); do
        grep -q "order entry on" "$out/planckd.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q "order entry on" "$out/planckd.log" || { echo "planckd did not start. See $out/planckd.log"; exit 1; }

    load_arguments=(--server "127.0.0.1:$base" --sessions 4 --orders "$orders" --csv)
    [ -n "$loadgen_cpu" ] && load_arguments+=(--cpu "$loadgen_cpu")

    echo "==> planck-loadgen, closed loop"
    "$bin/bench/planck-loadgen" "${load_arguments[@]}" 2> "$out/loadgen-closed.log" |
        sed 's/^order\./closed.order./' | tee "$out/loadgen-closed.csv" | pretty
    echo "==> planck-loadgen, ${rate} orders a second"
    "$bin/bench/planck-loadgen" "${load_arguments[@]}" --rate "$rate" --client-id 2000 \
        2> "$out/loadgen-rate.log" |
        sed 's/^order\./rate.order./' | tee "$out/loadgen-rate.csv" | pretty
    grep -h '^# sent' "$out"/loadgen-*.log || true
fi

echo
echo "results: $out"
if [ -n "$previous" ]; then
    echo "==> compared with ${previous%/}"
    python3 "$root/tools/bench_compare.py" "$previous" "$out" || true
fi
