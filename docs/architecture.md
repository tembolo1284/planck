# Architecture

How planck is put together and why. Read this before changing anything
structural. Choices that could still go another way are in
[decisions.md](decisions.md), and what is known to be wrong is in
[weaknesses.md](weaknesses.md). Ids like D-12, S-05 and W-70 point there.

## Principles

1. **One thread owns each piece of state.** A shard's books belong to that
   shard's thread and nobody else reads or writes them. There is no lock and
   no atomic between a command and its book (S-04).
2. **Threads talk only through rings.** Every ring has exactly one writer and
   one reader, and is wait-free.
3. **All memory is taken at startup.** Every table, pool and queue is carved
   from an arena before the first order arrives. Nothing on a hot path
   allocates, and nothing takes a page fault, because the pages were touched
   in advance by the thread that will use them.
4. **Never drop a message.** If a consumer falls behind, the producer waits.
   A venue that loses a fill is wrong. One that is briefly slow is only slow
   (S-05).
5. **One trust boundary.** Bytes from a socket are checked once, in the
   gateway. Past it, every field is trusted and nothing is checked again
   (S-09).
6. **No exceptions, no RTTI, bounded loops, contracts everywhere.** The NASA
   "Power of Ten" rules, adapted (S-02). See
   [contributing.md](contributing.md).

## Threads and rings

```
 clients                                                      subscribers
    |  TCP                                                         ^  UDP
    v                                                              |
+---------+  commands   +---------+  market events   +-----------+
| gateway | ----------> |  shard  | ---------------> | publisher | <-- retransmit
|         | <---------- |         |                  +-----------+     requests
+---------+   reports   |  books  |  journal entries +-----------+
                        +---------+ ---------------> | recorder  | --> shard-NN.journal
                                                     +-----------+
```

| Thread | Count | Owns | Does |
|--------|-------|------|------|
| shard | 1 to 16 | Order store, books for its symbols, its four rings | Matches. Nothing else. |
| gateway | 1 | Every client connection | Frames bytes, validates, routes commands in, copies reports out |
| publisher | 1 | The feed sockets and retransmit history | Numbers market events, packs them into datagrams, answers retransmit requests |
| recorder | 0 or 1 | The journal files | Drains journal rings to disk and syncs |

| Ring | Element | Size | Producer | Consumer |
|------|---------|-----:|----------|----------|
| commands | `Command`, 32 bytes | 65,536 | gateway | shard |
| reports | `Report`, 64 bytes | 65,536 | shard | gateway |
| market | `MarketEvent`, 48 bytes | 65,536 | shard | publisher |
| journal | `JournalEntry`, 48 bytes | off by default | shard | recorder |

Sizes are the defaults in `ShardConfig`. Each shard has its own set.

A symbol belongs to exactly one shard. `planckd` puts symbol S on shard
S mod N (D-10). Orders in different shards never interact, which is what
lets shards run in parallel with nothing shared.

## The life of an order

1. **Gateway reads** bytes from a client's socket and takes one cycle
   counter reading for the batch (`ingress_tsc`).
2. **Framing.** `wire::next_frame` finds one complete message. A length or
   type that is not in the protocol drops the connection.
3. **Trust boundary.** `wire::well_formed` checks every field: enums in
   range, reserved bytes zero. A malformed order is rejected to the client.
4. **Routing.** The symbol picks the shard. An unknown symbol is rejected
   here. For a cancel, the shard is read from the top four bits of the order
   id, so there is no lookup.
5. **Command ring.** The gateway builds a 32-byte `Command` and pushes it.
   If the ring is full the order is rejected as Busy at once (W-07).
6. **Shard pops** the command, reads its clock once, and gives the command
   that timestamp. Every message the command causes carries the same time.
7. **Matching.** `OrderBook::add` or `cancel`. Fills are reported through a
   callback that the compiler inlines.
8. **Encoding.** The shard writes each reply as a finished wire message into
   a stage: reports for the sessions involved, market events for the feed,
   and, if journaling, the command itself with its sequence and timestamp.
9. **Publishing.** Before taking the next command the shard moves the stage
   into the rings. If a ring is full it stops taking commands until there is
   room.
10. **Gateway picks up** the report, finds the session by id, and copies the
    already-encoded bytes into that session's send buffer. One `send()` per
    session per pass.
11. **Publisher picks up** the market event, gives it the stream's next
    sequence number, appends it to the current datagram and to the history.
12. **Recorder picks up** the journal entry, adds a checksum, and appends it
    to the shard's file.

The gateway measures steps 1 to 10: the report carries `ingress_tsc` back,
and the gateway records the difference in a histogram that `planckd --stats`
prints. That figure leaves out the kernel's network stack on both sides
(W-66).

## Modules

Bottom up. Each module may use only those above it in this list.

### core (`include/planck/core/`)

| File | Holds |
|------|-------|
| `strong.hpp`, `types.hpp` | `Strong<Tag, T>`: `Price`, `Qty`, `OrderId`, `SymbolId`, `SessionId`, `ShardId`, `Nanos`. A price cannot be passed where a quantity is wanted. Also `Side`, `OrderType`, `TimeInForce`. |
| `status.hpp` | `core::Status`, the result of every fallible call. `[[nodiscard]]`, so ignoring one does not compile. |
| `contract.hpp` | `PLANCK_EXPECT`, `PLANCK_ENSURE`, `PLANCK_ASSERT`, `PLANCK_VERIFY`. See [building.md](building.md) for the modes. |
| `config.hpp` | Hard limits: 16 shards, 4096 symbols, 1024 session slots, 64-byte cache line. |
| `bits.hpp`, `hint.hpp` | Power-of-two helpers, prefetch, `cpu_relax`. |

### sys (`include/planck/sys/`)

Everything that touches the operating system directly.

- **`clock.hpp`**: `tsc_now()` reads the cycle counter, about 20 cycles and
  no system call. `TscClock` converts ticks to nanoseconds after a 50 ms
  calibration against `CLOCK_MONOTONIC_RAW`, and anchors to the wall clock.
  `sample_wall()` reads counter and kernel clock as one instant, by
  bracketing the kernel read between two counter reads and keeping the
  tightest of several attempts.
- **`cpu.hpp`**: pin a thread, name it, check that its CPU is isolated, go
  real-time. `configure_hot_thread()` does all of it in one call.
- **`memory.hpp`**: `map_region()` with 2 MiB pages where available,
  `prefault()`, `lock_all_memory()`.

### mem (`include/planck/mem/`)

- **`Arena`**: a bump allocator over one mapped region. The only source of
  memory in planck. No free, no reset.
- **`IndexPool<T>`**: fixed-capacity pool addressed by 32-bit index, with the
  free list stored inside the free slots. Last released is first reused,
  because it is the one most likely still in cache.
- **`CacheAligned<T>`**: gives a value a cache line to itself.

### book (`include/planck/book/`)

The order book for one symbol, and the order store shared by all of a
shard's books.

**Price ladder.** Prices are integer ticks in a fixed band per symbol:
`[band_low, band_low + band_ticks)`, where `band_ticks` is a power of two
from 64 to 262,144 (S-06). The ladder is a flat array with one 16-byte
`Level` per tick. Finding a price is one subtraction and one index. Bids and
asks share the array, since in an uncrossed book every bid is below every
ask.

**Bitmaps.** One bit per tick per side says whether the level is occupied,
with summary levels above so that "next occupied level" is a few word
operations (`LadderBitmap`). Best bid and best ask are cached.

**Orders.** A resting order is 32 bytes, two to a cache line, in a pool
indexed by 32-bit slot. Each level is a doubly linked FIFO of slots. Add
without a match and cancel are O(1). A match is O(orders filled).

**Order ids** pack three things:

```
bits 60..63  shard     which matching thread owns the order
bits 24..59  sequence  counts up from 1, never reused
bits  0..23  slot      index of the order in the store
```

Lookup is one array read: go to the slot, compare the id stored there. No
hash table. The shard bits let the gateway route a cancel with no lookup at
all (S-07). An order that never rests gets a reserved slot value.

**Rules.** Price-time priority. Trades happen at the resting order's price.
Time in force is Day (rest), IOC (trade what you can, cancel the rest) or
FOK (all at once or nothing, checked before anything trades). A market order
is a limit order at the far edge of the band that never rests. An order that
may rest takes its slot before matching, because fills cannot be taken back
if no slot turned up afterwards. That has one odd consequence (W-23).

### ring (`include/planck/ring/spsc.hpp`)

`SpscRing<T>`: a bounded queue for exactly one producer thread and one
consumer thread. Wait-free: every call finishes in a bounded number of
steps.

- Producer fields and consumer fields sit on separate cache lines.
- Each side keeps a private copy of the other's index and re-reads the real
  one only when the copy says the queue looks full or empty. Most calls touch
  no memory the other thread writes.
- Indices count up forever and are masked on use, so there is no wrap logic
  and no wasted slot.
- `try_push_all` publishes a batch with one index update. `drain` consumes a
  batch with one.

### wire (`include/planck/wire/`)

The protocol. Every message is a fixed-size struct with no padding, so what
is in the struct is what is on the wire, and encode and decode are one
`memcpy`. Full reference in [protocol.md](protocol.md).

- `next_frame()` finds one message in a byte stream.
- `decode<T>()` copies it out and checks length and type.
- `well_formed()` checks the values inside. This is the trust boundary.

`static_assert`s pin every size and offset, so an accidental layout change
fails the build.

### engine (`include/planck/engine/`)

`Shard`: one matching thread and everything it owns. `messages.hpp` defines
what travels on the rings.

- **`init()`** must be called on the thread that will run the shard, so the
  memory it touches lands on that thread's NUMA node. It pins the thread if
  asked, maps and prefaults memory, builds the books and calibrates the
  clock.
- **`poll()`** handles up to 64 commands. It never blocks, with one
  exception: a single command whose output exceeds the 128-entry stage while
  a consumer's ring is full has to wait for room, because fills already
  reported cannot be undone. An order that sweeps many others does that.
- **`try_finish()`** carries on with a command that could not be finished
  in one go. Only a close can be like that. `poll()` calls it before taking
  each command.
- **`run()`** calls `poll()` until told to stop, spinning or yielding when
  idle.
- **`try_replay()`** carries out one journaled command with its original
  timestamp. It publishes market data as usual, reports to nobody, and does
  not journal again.

**Commands.** Four kinds: a new order, a cancel, one session closed, all
sessions closed. The last two come from the gateway, never from a client.

**Cancel on disconnect** (S-27, S-29). A shard asked for it
(`ShardConfig::cancel_on_disconnect`, which `planckd` always sets) keeps a
doubly linked list of resting orders for every session: two indices per
order slot in an array beside the order store, and a head per session id.
The shard links an order when it comes to rest and unlinks it when it is
cancelled or filled in full. The book knows nothing of this. When a session
closes, the shard walks its list, newest first, and cancels each order
through the book as if asked to: a `Canceled` with reason Disconnected for
the owner and an `MdDeleted` for the feed. "All sessions closed" does that
for every session id in turn. A shard not asked for it treats both commands
as no-ops, though it still counts and journals them.

A close can cancel far more orders than the stage holds, and unlike a sweep
it can stop anywhere. So it never waits: it cancels a stage-full, publishes,
and carries on, and if a ring is full `poll()` returns with the close
unfinished and resumes on the next call. No other command is taken until it
is done, and every cancel carries the one timestamp of the command, so the
outcome is the same however often it was interrupted. That matters to
replay, and to anything that drives a shard on a single thread (W-25).

**Backpressure.** If the report, market or journal ring is full, the shard
stops taking commands. Its command ring then fills, and the gateway starts
answering Busy. Nothing is dropped anywhere. A consumer that is dead, not
just slow, stalls the shard for good, and after 2^34 spins the process
aborts (W-21).

**Timestamps.** One clock reading per command, shared by every message that
command causes (D-08). The clock is re-anchored to the wall clock at most
once a second and only when the shard is idle. Timestamps are clamped so
they never go backwards, even if the wall clock does.

### net (`include/planck/net/`)

Thin, non-blocking wrappers. `Socket` owns a descriptor. `tcp_listen`,
`tcp_accept`, `tcp_connect`, `udp_open_sender`, `udp_open_receiver`.
`Poller` wraps epoll, level-triggered. Hot threads call it with a zero
timeout and never sleep in the kernel.

Two details that were learned the hard way:

- A plain UDP port may be bound by one socket only. Only multicast receivers
  share (F-06).
- A connected UDP sender that got "nobody there" reports it on the next
  send and does not perform that send. The publisher retries once (F-05).

### gateway (`include/planck/gateway/`)

One thread that owns every client connection.

- **Sessions and connections** (S-24). A session is a client's standing
  with the venue. Its id is the client id it logs in with, 1 to 65,535, the
  same on every connection and across restarts, and it is what owns orders
  in the shard and in the journal. A connection is one TCP link in one of
  1024 slots. A table maps each session id to the slot of the connection
  speaking for it, if any. That table is how a report finds its way out,
  and a report whose session has no connection is dropped and counted.
- **Disconnects** (S-27). When a connection that had logged in ends, the
  gateway pushes a "session closed" command to every shard, behind whatever
  that session had already sent. If a shard's ring is full the close is
  owed and goes out on a later pass, and until then that session may not
  log in again, or the late close would cancel its new orders. When the
  gateway itself starts, it pushes "all sessions closed" to every shard
  (S-28).
- **Buffers.** 4096 bytes in and 64 KiB out per session, all preallocated.
- **Slow clients.** A client whose out buffer fills is disconnected. Holding
  its backlog would mean unbounded memory or stalling everyone (D-03).
- **Per pass:** up to 8 accepts, 4 reads per ready socket, 256 reports per
  shard, one send per session with something to say.
- **Silence** (S-31). Each connection carries the time it was last heard
  from and last sent to, on the gateway's own clock, read once a pass. A
  few times in each interval the gateway looks over every slot: it ends a
  connection not heard from for the idle timeout, or not logged in by
  then, and it sends a `Heartbeat` to a client sent nothing for the
  heartbeat interval. If two passes are 100 ms or more apart the gateway
  itself was held up, and everyone gets a fresh start: what they sent
  meanwhile has not been read yet. Off unless configured. `planckd` turns
  it on.
- **Login** checks the protocol version, that the client id is in range,
  that it is listed with that token if the venue has a clients file
  (`Credentials`, S-23), and that no other connection holds the session. A
  refused login is told why and disconnected (S-25).

### feed (`include/planck/feed/`)

**Publisher.** One UDP stream per shard, each with its own sequence numbers
from 1. Messages are packed several to a datagram, never split, and a
datagram stays under 1400 bytes so it does not fragment. A quiet stream
sends a heartbeat every second carrying the next sequence number, so a
subscriber can tell silence from loss. The last 65,536 messages of each
stream are kept, and any range can be re-requested over UDP, up to 512
messages per request.

**Receiver.** The subscriber's side. Delivers each message exactly once and
strictly in sequence. On a gap it stops, asks for a resend from the first
missing message, and discards anything newer until the hole is filled
(go-back-N, D-04). After 100 unanswered requests, 2 ms apart, it declares
the gap lost and carries on, and `stats().lost` going up means its books
are no longer complete (W-10).

### perf (`include/planck/perf/histogram.hpp`)

`Histogram`: 1920 fixed buckets, 32 per power of two, error under about 3
percent at any magnitude, 15 KiB. `record()` is a shift, an add and an
increment. One writer, no locking.

### journal (`include/planck/journal/`)

The journal records **commands, not effects** (S-12). Matching is
deterministic, so replaying the same commands with the same timestamps into
an empty shard rebuilds the same books and publishes the same market data,
byte for byte. The shard writes the journal, not the gateway, because only
the shard knows the order it handled commands in and the time it gave each.

- **`format.hpp`**: one file per shard, a 64-byte header then 48-byte
  records, each with a CRC-32C. Layout in [protocol.md](protocol.md).
- **`Writer`**: appends. On opening an existing file it reads it all, cuts
  off a torn last record, and refuses a file with damage in the middle or
  from a different configuration.
- **`Reader`**: reads and checks, and can tell a torn tail from damage with
  good records after it.
- **`Recorder`**: the thread. Drains journal rings into the files and syncs
  at least every `sync_interval` (10 ms).
- **`replay()`**: rebuilds a fresh shard from its file.
- **`fingerprint()`**: a hash of everything that affects matching (shard id,
  order capacity, symbols and bands). A journal only replays into the
  configuration that wrote it (W-74).

**Durability is asynchronous** (D-12). The shard answers first and the
recorder writes after. A process crash loses nothing, since the kernel has
the data. A power cut can lose up to one sync interval of commands that
clients were already told about (W-70). If the disk fails, matching carries
on and the journal stops being true (W-71, D-13).

### apps, bench, capi

Outside the library. See [operations.md](operations.md),
[benchmarks.md](benchmarks.md) and [c_api.md](c_api.md).

## Startup and shutdown in planckd

Order matters, and `apps/planckd.cpp` is the reference.

**Start:**

1. Each shard thread starts and calls `Shard::init()` on itself.
2. The publisher starts. It must be running before replay, because replay
   publishes market data and something has to drain it.
3. Each shard replays its journal, if there is one, on its own thread.
4. The recorder starts. It checks that each journal's next sequence number
   is the one its shard will produce.
5. The gateway starts. Its first act is to tell every shard that all
   sessions are closed, which cancels every order the journals rebuilt
   (S-28). Those cancels are journaled and go out on the feed.
6. `ready` is printed.

**Stop** (SIGINT or SIGTERM): shards first, then the gateway and publisher,
then the recorder, which writes and syncs whatever is left. Orders in flight
when the shards stop get no answer (W-61).

## Limits

| What | Limit | Where |
|------|------:|-------|
| Shards | 16 | `core::kMaxShards` |
| Symbol ids | 0 to 4095 | `core::kMaxSymbols` |
| Session slots | 1024 | `core::kMaxSessions` |
| Client ids, which are session ids | 1 to 65,535 | 16-bit `SessionId` |
| Clients in a clients file | 4,096 | `Credentials::kMaxClients` |
| Clients connected at once | 256 by default | `GatewayConfig::max_sessions` |
| Connections over a run | no limit | |
| Resting orders per shard | 1,048,576 by default, 16,777,215 at most | `ShardConfig::order_capacity`, 24 slot bits |
| Orders per shard, ever | about 68 billion | 36 sequence bits |
| Price band | 64 to 262,144 ticks, a power of two | `OrderBook` |
| Price | signed 32-bit ticks | `core::Price` |
| Quantity | unsigned 32-bit | `core::Qty` |
| Largest wire message | 48 bytes | `wire::kMaxMessageSize` |
| Largest datagram | 1400 bytes | `wire::kMaxDatagramSize` |
| Feed history per stream | 65,536 messages by default | `PublisherConfig` |

## What would change the shape

A few open decisions would reach across modules if taken. Know about them
before designing on top of the current shape.

- **Keeping orders through a disconnect (D-18)** needs report recovery
  first, so a returning client can learn what happened to them.
- **A limit on resting orders per session (D-19)** is a check where an
  order comes to rest, and a counter beside each session's list.
- **Group commit (D-12)** means holding reports until the sync that covers
  them: a path from recorder back to gateway.
- **Feed snapshots (D-05)** and **journal snapshots (W-72)** are the same
  mechanism, book state as of a sequence number, and should be built once.
- **More than one gateway (W-06)** needs one command ring per gateway per
  shard, because the rings are single producer.
- **Order modification (D-07)** is a third command kind through gateway,
  shard, book, journal and wire.
