#!/usr/bin/env bash
# Checks that the benchmark programs run and that their figures add up. It
# says nothing about speed: it uses a few thousand samples and whatever
# build it is given.
#
#   bench_smoke.sh <directory holding planckd> <directory holding planck-bench>
set -u

apps="${1:?usage: bench_smoke.sh <apps directory> <bench directory>}"
bench="${2:?usage: bench_smoke.sh <apps directory> <bench directory>}"
work="$(mktemp -d)"
venue_pid=""

cleanup() {
    [ -n "$venue_pid" ] && kill "$venue_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    for log in "$work"/*.log; do
        echo "---- $(basename "$log")"
        cat "$log"
    done
    exit 1
}

expect() {
    grep -Eq -- "$2" "$work/$1" || fail "$3 (no '$2' in $1)"
}

wait_for() {
    for _ in $(seq 1 100); do
        grep -Eq -- "$2" "$work/$1" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "timed out waiting for '$2' in $1"
}

# ---- in process ----

"$bench/planck-bench" --quick --csv > "$work/bench.log" 2> "$work/bench-err.log" \
    || fail "planck-bench did not finish cleanly"
expect bench.log "^timer.empty_interval,200000," "the timer's own cost must be reported"
for row in book.add book.cancel book.match_1_fill book.match_10_fills \
           ring.push_pop ring.core_to_core wire.decode_new_order \
           shard.rest shard.cancel shard.trade \
           shard.rest.journal shard.cancel.journal shard.trade.journal \
           shard_thread.rest shard_thread.cancel shard_thread.trade; do
    expect bench.log "^$row,3000," "benchmark $row is missing or short of samples"
done
[ "$(wc -l < "$work/bench.log")" -eq 18 ] || fail "expected a header and 17 rows"

"$bench/planck-bench" --quick --filter book.match > "$work/filter.log" 2>> "$work/bench-err.log" \
    || fail "planck-bench --filter failed"
[ "$(grep -c '^book\.match' "$work/filter.log")" -eq 2 ] || fail "--filter did not pick the two match rows"
if grep -q '^ring\|^shard' "$work/filter.log"; then fail "--filter let other benchmarks through"; fi

"$bench/planck-bench" --filter no-such-thing > "$work/none.log" 2>&1
[ $? -eq 2 ] || fail "an unknown benchmark name must be an error"
"$bench/planck-bench" --samples nonsense > "$work/bad.log" 2>&1
[ $? -eq 2 ] || fail "a bad option must be an error"

# ---- against a venue ----

base=$((28000 + ($$ % 3000) * 4))
"$apps/planckd" --listen "127.0.0.1:$base" --feed "127.0.0.1:$((base + 1))" \
    --retransmit "127.0.0.1:$((base + 3))" --shards 2 --symbols 4 --stats 1 \
    > "$work/venue.log" 2>&1 &
venue_pid=$!
wait_for venue.log "order entry on"

# Closed loop.
"$bench/planck-loadgen" --server "127.0.0.1:$base" --sessions 3 --orders 3000 --warmup 300 \
    --symbols 4 --depth 40 --cross 20 --csv > "$work/closed.log" 2> "$work/closed-err.log" \
    || fail "planck-loadgen (closed loop) did not finish cleanly"
expect closed.log "^order.sent_to_answer,3000," "closed loop must time exactly the orders asked for"
if grep -q "due_to_answer" "$work/closed.log"; then fail "closed loop has no schedule to measure against"; fi
expect closed-err.log "sent 3300, accepted 3300, rejected 0 " "every order, warm-up included, must be accepted"
expect closed-err.log "unexpected 0$" "the venue sent something the generator did not expect"
expect closed-err.log "most unanswered 1," "closed loop with a window of 1 sent before it was answered"

"$bench/planck-loadgen" --server "127.0.0.1:$base" --orders 2000 --warmup 0 --window 8 \
    --symbols 4 --client-id 4000 > "$work/window.log" 2> "$work/window-err.log" \
    || fail "planck-loadgen (window 8) did not finish cleanly"
expect window-err.log "most unanswered 8," "a window of 8 must be used and not exceeded"

# Fixed rate.
"$bench/planck-loadgen" --server "127.0.0.1:$base" --sessions 2 --orders 2000 --warmup 100 \
    --rate 4000 --symbols 4 --client-id 5000 --csv > "$work/rate.log" 2> "$work/rate-err.log" \
    || fail "planck-loadgen (fixed rate) did not finish cleanly"
expect rate.log "^order.sent_to_answer,2000," "fixed rate must time exactly the orders asked for"
expect rate.log "^order.due_to_answer,2000," "fixed rate must also measure from the due time"
expect rate-err.log "fixed rate" "the summary must say how the load was paced"
# 2000 orders at 4000 a second take half a second, give or take scheduling.
expect rate-err.log "timed in 0\.[4-9][0-9]* s" "the fixed rate was not kept"

# A rate nothing could keep: ten million a second. Every order goes out
# late, most of them by milliseconds. The time measured from when each was
# due must therefore average well above the time measured from when it was
# sent. The gap between the two averages is exactly the average lateness. If
# it is missing, the waiting that a fixed rate exists to expose is not being
# counted.
"$bench/planck-loadgen" --server "127.0.0.1:$base" --orders 4000 --warmup 0 --rate 10000000 \
    --symbols 4 --client-id 6000 --csv > "$work/late.log" 2> "$work/late-err.log" \
    || fail "planck-loadgen (impossible rate) did not finish cleanly"
sent_mean="$(awk -F, '$1 == "order.sent_to_answer" { print $10 }' "$work/late.log")"
due_mean="$(awk -F, '$1 == "order.due_to_answer" { print $10 }' "$work/late.log")"
awk -v sent="$sent_mean" -v due="$due_mean" 'BEGIN { exit !(sent > 0 && due - sent > 50000) }' \
    || fail "late orders were not measured as late (mean from sent $sent_mean ns, from due $due_mean ns)"

# Every run cancelled what it left resting, so the books end empty. The
# venue prints its counters once a second: look at the newest line only,
# since the lines from before the load say zero as well.
emptied=no
for _ in $(seq 1 50); do
    if grep "^stats engine" "$work/venue.log" | tail -n 1 | grep -q "resting=0 "; then
        emptied=yes
        break
    fi
    sleep 0.1
done
[ "$emptied" = yes ] || fail "orders were left resting after the load generators finished"
grep "^stats engine" "$work/venue.log" | tail -n 1 | grep -Eq "commands=[1-9]" \
    || fail "the venue's counters never showed the load"

# A venue that is not there.
"$bench/planck-loadgen" --server "127.0.0.1:$((base + 2))" --orders 10 > "$work/nobody.log" 2>&1
[ $? -eq 1 ] || fail "connecting to nobody must fail"
expect nobody.log "cannot connect" "the failure must say why"

echo "PASS"
