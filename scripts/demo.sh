#!/usr/bin/env bash
# demo.sh: a venue, a market data viewer and two clients, on this machine.
#
#   scripts/demo.sh [directory holding the apps]
#
# Starts planckd on loopback, attaches planck-mdtap to its feed, and has two
# clients trade with each other. Prints what each of them saw. With no
# argument it uses build/gcc-release, building it first if need be.
#
# To type at a client yourself afterwards:
#   build/gcc-release/apps/planckd &
#   build/gcc-release/apps/planck-client --client-id 1
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ $# -ge 1 ]; then
    bin="$1"
else
    "$root/build.sh" build --release > /dev/null
    bin="$root/build/gcc-release/apps"
fi

work="$(mktemp -d)"
pids=()
cleanup() {
    for pid in ${pids[@]+"${pids[@]}"}; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

wait_for() {
    for _ in $(seq 1 100); do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "demo.sh: timed out waiting for '$2'"; cat "$1"; exit 1
}

show() {
    echo
    echo "---- $1"
    sed 's/^/    /' "$2"
}

base=$((44000 + ($$ % 2000) * 4))
"$bin/planckd" --listen "127.0.0.1:$base" --feed "127.0.0.1:$((base + 1))" \
    --retransmit "127.0.0.1:$((base + 2))" --symbols 1 --shards 1 > "$work/venue" 2>&1 &
pids+=($!)
wait_for "$work/venue" "^ready$"

"$bin/planck-mdtap" --stream 0 --listen "127.0.0.1:$((base + 1))" --count 6 > "$work/tap" 2>&1 &
pids+=($!)
wait_for "$work/tap" "^listening"

# The maker offers 10 at 1000 and 5 more at 1001, then waits to hear them trade.
printf 'sell 1 10 1000\nsell 1 5 1001\nwait 1500\n' |
    "$bin/planck-client" --connect "127.0.0.1:$base" --client-id 1 > "$work/maker" 2>&1 &
maker=$!
pids+=("$maker")
wait_for "$work/maker" "client_order_id=2"

# The taker buys 12 at up to 1001: all 10 at 1000, then 2 of the 5 at 1001.
# Then it bids 3 at 990, which rests, and thinks better of it.
printf 'buy 1 12 1001 ioc\nbuy 1 3 990\ncancel last\n' |
    "$bin/planck-client" --connect "127.0.0.1:$base" --client-id 2 > "$work/taker" 2>&1

wait "$maker" || true
wait_for "$work/tap" "^summary"

show "the venue" "$work/venue"
show "client 1, the maker: sells 10 at 1000 and 5 at 1001" "$work/maker"
show "client 2, the taker: buys 12 up to 1001, then bids and cancels" "$work/taker"
show "the market data feed, as anyone could see it" "$work/tap"
