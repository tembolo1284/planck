#!/usr/bin/env bash
# End-to-end check of the three programs as separate processes.
#
#   apps_smoke.sh <directory holding planckd, planck-client and the rest>
#
# Starts a venue on loopback, trades through it with two clients, and checks
# what each client was told, what appeared on the market data feed, and that
# a late subscriber can replay the feed. A client that leaves must have its
# resting orders cancelled. Then kills the venue outright, starts it again
# from its journal, and checks that the feed carried over, that the orders
# left resting by the crash are cancelled, and that sessions are still the
# same clients. The venue runs with a clients file throughout, so who may
# log in is checked as well. Last, a client whose line goes dead without
# closing must be noticed and its order cancelled.
set -u

bin="${1:?usage: apps_smoke.sh <bin directory>}"
work="$(mktemp -d)"
venue_pid=""
maker_pid=""
tap_pid=""

cleanup() {
    for pid in "$tap_pid" "$maker_pid" "$venue_pid"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
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

# expect <file> <pattern> <what it means>
expect() {
    grep -Eq -- "$2" "$work/$1" || fail "$3 (no '$2' in $1)"
}

# Wait up to 10 seconds for a pattern to show up in a file.
wait_for() {
    for _ in $(seq 1 100); do
        grep -Eq -- "$2" "$work/$1" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "timed out waiting for '$2' in $1"
}

# Four ports that tests running side by side are unlikely to share. They sit
# below the range the kernel hands out on its own.
base=$((12000 + ($$ % 4000) * 4))
order_port=$base
quiet_feed_port=$((base + 1))   # stream 0: shard 0, which trades symbol 2
feed_port=$((base + 2))         # stream 1: shard 1, which trades symbol 1
retransmit_port=$((base + 3))

# Only these three may log in, each with its own token.
printf '# client token\n1 1001\n2 0x7d2   # 2002\n3 3003\n' > "$work/clients"

# Two shards, so that routing by symbol and one feed stream per shard are
# both exercised. Symbol S trades on shard S mod 2.
start_venue() {
    "$bin/planckd" --clients "$work/clients" --listen "127.0.0.1:$order_port" \
        --feed "127.0.0.1:$quiet_feed_port" \
        --retransmit "127.0.0.1:$retransmit_port" --shards 2 --symbols 4 --orders 4096 \
        --huge off --journal "$work/journal" --heartbeat 400 --idle-timeout 2000 \
        > "$work/$1" 2>&1 &
    venue_pid=$!
    wait_for "$1" '^ready$'
}
start_venue venue.log
expect venue.log '^shard 1 replayed 0 commands$' "a first start found something to replay"
expect venue.log '^clients: 3 listed in ' "the venue did not read its clients file"
expect venue.log '^clients: one heard nothing from for 2000 ms is disconnected$' \
    "the venue did not take its idle timeout"

# A live subscriber, there from the first message.
"$bin/planck-mdtap" --stream 1 --listen "127.0.0.1:$feed_port" --count 4 \
    > "$work/tap.log" 2>&1 &
tap_pid=$!
wait_for tap.log '^listening for stream 1 '

# The maker rests an order and stays connected to hear it trade.
printf 'sell 1 10 1000\nwait 300000\n' |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 1 --token 1001 \
    > "$work/maker.log" 2>&1 &
maker_pid=$!
wait_for maker.log '^accepted client_order_id=1 '

# The taker trades with it, rests an order of its own and cancels that, is
# refused twice, rests an order in the other shard's symbol, and leaves.
printf 'buy 1 4 1000\nbuy 1 3 990\ncancel last\nbuy 9 1 1000\nbuy 1 1 999999\nsell 2 5 500\n' |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 2 --token 2002 \
    > "$work/taker.log" 2>&1 || fail "the taker exited with an error"

expect taker.log '^logged in client_id=2 ' "the taker never logged in"
expect taker.log '^executed .* qty=4 price=1000 leaves=0 taker ' "the taker was not told of its fill"
expect taker.log '^canceled .* qty=3 reason=requested ' "the cancel was not confirmed"
expect taker.log '^rejected client_order_id=3 reason=UnknownSymbol$' "an unknown symbol was not rejected"
expect taker.log '^rejected client_order_id=4 reason=PriceOutOfBand$' "a price outside the band was not rejected"
# An order id carries its shard in the top four bits: 2^60 and up is shard 1.
expect taker.log '^accepted client_order_id=1 order_id=11529215[0-9]{11} ' "symbol 1 was not matched on shard 1"
expect taker.log '^accepted client_order_id=5 order_id=[0-9]{1,12} ' "symbol 2 was not matched on shard 0"

wait_for maker.log '^executed .* qty=4 price=1000 leaves=6 maker '

# Stream 1: one add, one trade, one add, one delete, in that order. The order
# in symbol 2 belongs to stream 0 and must not show up here.
wait_for tap.log '^summary '
wait "$tap_pid" || fail "the live subscriber exited with an error"
tap_pid=""
expect tap.log '^1 add symbol=1 sell qty=10 price=1000 ' "feed message 1 is wrong"
expect tap.log '^2 trade symbol=1 qty=4 price=1000 ' "feed message 2 is wrong"
expect tap.log '^3 add symbol=1 buy qty=3 price=990 ' "feed message 3 is wrong"
expect tap.log '^4 delete symbol=1 ' "feed message 4 is wrong"
expect tap.log '^summary messages=4 .* lost=0$' "the live subscriber lost messages"

# A subscriber that arrives after it all happened replays it from the venue.
# It learns that it is behind from the stream's next heartbeat.
timeout 20 "$bin/planck-mdtap" --stream 1 --listen "127.0.0.1:$feed_port" \
    --retransmit "127.0.0.1:$retransmit_port" --from-start --count 4 \
    > "$work/late.log" 2>&1 || fail "the late subscriber did not finish"
expect late.log '^1 add symbol=1 sell qty=10 price=1000 ' "the replay is missing message 1"
expect late.log '^4 delete symbol=1 ' "the replay is missing message 4"
expect late.log '^summary messages=4 .* lost=0$' "the replay lost messages"

# The taker has gone, so the order it left in symbol 2 is cancelled: stream 0
# shows it arrive and then go, with nobody having asked.
timeout 20 "$bin/planck-mdtap" --stream 0 --listen "127.0.0.1:$quiet_feed_port" \
    --retransmit "127.0.0.1:$retransmit_port" --from-start --count 2 \
    > "$work/left.log" 2>&1 || fail "the order of a client that left was not cancelled"
expect left.log '^1 add symbol=2 sell qty=5 price=500 ' "the taker's last order never rested"
expect left.log '^2 delete symbol=2 ' "the order of a client that left is still in the book"

# A second login under an id that is in use is refused.
printf 'ping\n' |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 1 --token 1001 \
    > "$work/twin.log" 2>&1 && fail "a duplicate client id was allowed to log in"
expect twin.log '^login rejected reason=AlreadyLoggedIn$' "the duplicate login got the wrong answer"

# Nor does anyone get in with the wrong token, or with an id that is not listed.
printf 'buy 1 1 1000\n' |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 3 --token 1001 \
    > "$work/thief.log" 2>&1 && fail "a client logged in with another client's token"
expect thief.log '^login rejected reason=BadCredentials$' "a wrong token got the wrong answer"
printf 'buy 1 1 1000\n' |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 4 \
    > "$work/stranger.log" 2>&1 && fail "a client that is not listed logged in"
expect stranger.log '^login rejected reason=BadCredentials$' "an unlisted client got the wrong answer"

# ---- The venue dies without warning ----

# Wait until the journal holds everything, then kill the venue outright. No
# signal handler runs and nothing is flushed on the way out.
journal_holds() {
    "$bin/planck-replay" --file "$work/journal/shard-01.journal" > "$work/replay.log" 2>&1
    grep -q '^commands=7 ' "$work/replay.log"
}
for _ in $(seq 1 100); do
    journal_holds && break
    sleep 0.1
done
journal_holds || fail "the journal never received the seven commands shard 1 handled"
expect replay.log '^intact$' "the journal is not intact"
kill -KILL "$venue_pid"
wait "$venue_pid" 2>/dev/null
venue_pid=""
kill "$maker_pid" 2>/dev/null
wait "$maker_pid" 2>/dev/null
maker_pid=""

"$bin/planck-replay" --file "$work/journal/shard-01.journal" --dump > "$work/dump.log" 2>&1 ||
    fail "the journal did not survive the kill"
expect dump.log '^1 .* all sessions closed$' "a venue must open by closing every session"
expect dump.log '^2 .* session=1 sell symbol=1 qty=10 price=1000 day ' "journal record 2 is wrong"
expect dump.log '^5 .* session=2 cancel order_id=' "journal record 5 is wrong"
expect dump.log '^6 .* session=2 buy symbol=1 qty=1 price=999999 day ' \
    "a command the book rejected must still be in the journal"
expect dump.log '^7 .* session=2 closed$' "the taker's departure is not in the journal"

# ---- And comes back ----

start_venue venue2.log
expect venue2.log '^shard 1 replayed 7 commands$' "shard 1 was not rebuilt from its journal"
expect venue2.log '^shard 0 replayed 3 commands$' "shard 0 was not rebuilt from its journal"

# A client whose line goes dead. It logs in as client 2 and rests an order
# in symbol 2, by hand, byte by byte, and then says nothing more: no logout,
# no close, no heartbeat. The connection stays open until the end of this
# script. The venue must give up on it by itself.
exec 9<>"/dev/tcp/127.0.0.1/$order_port" || fail "could not open a raw connection"
# LoginRequest: length 40, type 1, protocol 3, client 2, token 2002, and no
# earlier reports asked for.
printf '\x28\x00\x01\x00\x03\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00' >&9
printf '\xd2\x07\x00\x00\x00\x00\x00\x00' >&9
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >&9
# NewOrder: length 32, type 0x10, symbol 2, buy, limit, id 6, price 400, qty 5,
# day. The id is 6 because client 2 used 1 to 5 before the restart, and the
# venue knows that from its journal. The order is sent twice, as a client
# unsure whether it arrived might: the second must be ignored.
for _ in 1 2; do
    printf '\x20\x00\x10\x00\x02\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00' >&9
    printf '\x90\x01\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >&9
done

# The maker's order had 6 left when the venue died, and the journal brought
# it back. But nobody is connected to a venue that has just started, so it
# was cancelled before the first client could log in. The maker comes back
# as the same session, asks for whatever reports the venue holds for it, and
# so learns of that cancel. Then it starts afresh, with order ids that carry
# on above the one it used before the restart.
maker_order="$(sed -n 's/^accepted client_order_id=1 order_id=\([0-9]*\) .*/\1/p' "$work/maker.log")"
[ -n "$maker_order" ] || fail "could not find the maker's order id"
printf 'cancel %s\nsell 1 2 1000\nwait 300000\n' "$maker_order" |
    "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 1 --token 1001 \
    --next-report 1 > "$work/maker2.log" 2>&1 &
maker_pid=$!
wait_for maker2.log '^accepted client_order_id=2 '
expect maker2.log '^logged in client_id=1 session=1$' "a session id must be the client id"
expect maker2.log '^recovering reports 1 to 1$' "the maker was not offered the report it missed"
expect maker2.log "^earlier #1: canceled order_id=$maker_order qty=6 reason=disconnected " \
    "the maker was not told that the restart cancelled its order"
expect maker2.log '^caught up$' "the maker never finished recovering"
expect maker2.log "^cancel rejected order_id=$maker_order reason=UnknownOrder\$" \
    "an order from before the restart was still there to cancel"

# Another client trades with the new order and rests what is left over. It
# is told to stay until the venue has been quiet for a second, which is
# longer than the venue goes between heartbeats: they must not keep it.
printf 'buy 1 6 1000\n' |
    timeout 10 "$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 3 \
    --token 3003 --linger 1000 \
    > "$work/after.log" 2>&1 || fail "the client did not leave, or left with an error"
expect after.log '^executed .* qty=2 price=1000 leaves=4 taker ' \
    "only the 2 offered after the restart may trade, not the 6 from before it"
wait_for maker2.log '^executed .* qty=2 price=1000 leaves=0 maker '

# A subscriber replaying from the start sees the first life's messages
# exactly as they were. Then the restart's cancel, the new order, the trade,
# what client 3 left resting, and that being cancelled when client 3 left.
timeout 20 "$bin/planck-mdtap" --stream 1 --listen "127.0.0.1:$feed_port" \
    --retransmit "127.0.0.1:$retransmit_port" --from-start --count 9 \
    > "$work/reborn.log" 2>&1 || fail "the subscriber did not finish after the restart"
for line in 1 2 3 4; do
    before="$(grep "^$line " "$work/late.log")"
    after="$(grep "^$line " "$work/reborn.log")"
    [ -n "$before" ] && [ "$before" = "$after" ] ||
        fail "feed message $line changed across the restart: '$before' became '$after'"
done
expect reborn.log "^5 delete symbol=1 order_id=$maker_order " \
    "the order left by the crash was not cancelled at the restart"
expect reborn.log '^6 add symbol=1 sell qty=2 price=1000 ' "feed message 6 is wrong"
expect reborn.log '^7 trade symbol=1 qty=2 price=1000 ' "the trade after the restart is not on the feed"
expect reborn.log '^8 add symbol=1 buy qty=4 price=1000 ' "feed message 8 is wrong"
expect reborn.log '^9 delete symbol=1 ' "the order of the client that left is still in the book"

# Client 3 comes back and asks for what it missed, with the numbers it was
# given when it left. It had seen two reports. The third is the cancel of
# the order it left behind, which nobody was connected to hear.
left_at="$(sed -n 's/^left at //p' "$work/after.log")"
[ -n "$left_at" ] || fail "the client did not say where it left off"
# shellcheck disable=SC2086
"$bin/planck-client" --connect "127.0.0.1:$order_port" --client-id 3 --token 3003 $left_at \
    < /dev/null > "$work/again.log" 2>&1 || fail "the returning client exited with an error"
expect again.log '^recovering reports 3 to 3$' "the returning client was not offered what it missed"
expect again.log '^earlier #3: canceled .* qty=4 reason=disconnected ' \
    "the returning client was not told that its order was cancelled"
expect again.log '^left at --epoch [0-9]+ --next-report 4$' "the returning client lost count"

# By now the silent client has had its two seconds. Stream 0 shows the
# first life's order come and go, then the silent client's order arrive,
# and then go, with nobody having said a word.
timeout 20 "$bin/planck-mdtap" --stream 0 --listen "127.0.0.1:$quiet_feed_port" \
    --retransmit "127.0.0.1:$retransmit_port" --from-start --count 4 \
    > "$work/dead.log" 2>&1 || fail "the order of a client that went silent was never cancelled"
expect dead.log '^3 add symbol=2 buy qty=5 price=400 ' "the silent client's order never rested"
expect dead.log '^4 delete symbol=2 ' \
    "the silent client's order is still in the book, or its repeat rested as well"

# The maker has been connected all that time with nothing to say, and is
# still there: a real client sends heartbeats without being asked.
kill -0 "$maker_pid" 2>/dev/null || fail "a quiet but living client was dropped"
kill "$maker_pid" 2>/dev/null || true
wait "$maker_pid" 2>/dev/null || true
maker_pid=""

# Wait for the venue to have dealt with the maker leaving too: command 15
# of shard 1, counting both lives.
for _ in $(seq 1 100); do
    "$bin/planck-replay" --file "$work/journal/shard-01.journal" > "$work/replay2.log" 2>&1
    grep -q '^commands=15 ' "$work/replay2.log" && break
    sleep 0.1
done
expect replay2.log '^commands=15 new_orders=6 cancels=2 closes=7 ' \
    "the journal of shard 1 does not hold both lives"

# Clean shutdown on SIGTERM, with final counters that match both lives.
kill -TERM "$venue_pid"
wait "$venue_pid" || fail "the venue did not exit cleanly"
venue_pid=""
expect venue2.log '^stopping$' "the venue did not notice the signal"
expect venue2.log '^stats engine commands=24 accepted=7 rejected=2 trades=2 cancels=1 disconnect_cancels=4 resting=0 ' \
    "the venue's final counters are wrong"
expect venue2.log '^stats feed messages=13 ' "the venue's feed counters are wrong"
expect venue2.log '^stats journal records=14 .* discarded=0 failed=0$' \
    "the journal did not record the fourteen commands of the second life"
# Every one of them was timed from the recorder taking it to its sync.
expect venue2.log '^stats durable records=14 p50_ns=[0-9]+ .* sync_max_ns=[0-9]+$' \
    "the venue did not report how long its records took to be safe"
expect venue2.log '^stats gateway .* idle_timeouts=1 login_timeouts=0 ' \
    "the venue did not count the client it gave up on"
expect venue2.log '^stats gateway .* recovered=2 recoveries=2 recovery_gaps=0 duplicate_orders=1$' \
    "the venue did not count the reports it sent again, or the order it ignored"
exec 9>&-
expect venue2.log '^stats latency reports=' "no latency was measured"

echo "PASS"
