# Decisions

Choices that shape planck and could reasonably have gone another way. The
open ones are being settled one at a time, in the order under "Agreed plan".
Until one is settled the code does what "Today" says.

Things that are simply broken or missing live in `weaknesses.md`.

## Agreed plan

Decided on 2026-11-26. Each step ends with its tests, its seeded bugs and
these documents brought up to date.

| Step | What | Covers | State |
|-----:|------|--------|-------|
| 1 | Sessions that outlive connections, and a list of who may log in | D-02, W-02, W-05 | done: S-23, S-24 |
| 2 | Cancel on disconnect, and at restart | D-01, D-14, W-01 | done: S-27, S-28 |
| 3a | Heartbeats, and dropping a connection that has gone silent | W-03 | done: S-31, S-32 |
| 3b | Report sequence numbers, report recovery, idempotent orders | W-04 | done: S-33, S-34, S-35 |
| 4 | A commit boundary: nothing is told to anyone before it is durable. Measure the cost first. Held reports that survive a restart. | D-12, D-13, W-70, W-71, W-80, W-98 | next |
| 5 | Feed snapshots and a separate, rate-limited recovery service | D-05, W-10, W-11, W-13 | |
| 6 | A failed market data consumer must not stop matching | W-21 | |
| any time | Measure on real hardware | W-50 | |

Replicating commands to a standby engine comes after step 4, as a project
of its own.

## Open

### D-03 Slow order entry clients
- **Today:** a client with more than 64 KiB of unread reports is disconnected.
- **Alternative:** a larger or per-client buffer. Stalling the gateway for one client is not an option.
- **Leaning:** keep, and make the size part of the client's configuration.

### D-04 Feed gap recovery
- **Today:** go-back-N. Simple and always correct, wasteful under loss.
- **Alternative:** buffer out-of-order datagrams and request only the holes.
- **Leaning:** keep until snapshots exist (D-05), then decide with loss measurements.
- **Touches:** W-12.

### D-05 Snapshots for the feed
- **Today:** none. A subscriber must hear the stream from sequence 1 or from within the retransmit history.
- **Alternatives:** a periodic snapshot on its own multicast group, or a snapshot on request over TCP.
- **Leaning:** on request over TCP. It is simpler, and a late joiner is not latency sensitive.
- **Touches:** W-10.

### D-06 Self-trade prevention
- **Today:** none. Two orders from the same session match each other like any others.
- **Alternatives:** cancel the resting order, cancel the incoming order, or leave it to clients.
- **Leaning:** undecided. It adds a compare to the matching loop.

### D-07 Order modification
- **Today:** new and cancel only. A client changes an order by cancelling it and sending another, and loses queue position.
- **Alternative:** a reduce-quantity message that keeps position, and perhaps a full replace.
- **Leaning:** add reduce-quantity. Leave replace out.

### D-08 Timestamps
- **Today:** one reading per command from the counter-based clock, shared by every message the command causes. Re-anchored to the wall clock when idle, clamped so it never goes backwards.
- **Alternative:** discipline the rate continuously against the wall clock, or stamp with a hardware clock.
- **Leaning:** keep. Revisit once W-30 is understood.

### D-10 How the venue learns its symbols
- **Today:** `planckd --symbols N` makes symbols 1 to N, all with the same price band, and puts symbol S on shard S mod the shard count.
- **Alternative:** a config file with one line per symbol: id, name, band, shard.
- **Leaning:** the config file, written when `journal/` is done, since the journal will want to record the same table.
- **Touches:** W-60.

### D-11 What an unpinned venue does when idle
- **Today:** without `--cpus`, every thread sleeps 50 microseconds when it has nothing to do. A request crosses three such threads, so a round trip costs a few hundred microseconds. With `--cpus` the threads spin.
- **Alternative:** yield, which is faster but keeps every thread at 100 percent of a core.
- **Leaning:** keep. An unpinned venue is for development, and `--idle yield` or `--idle spin` is one flag away.

### D-12 When a command counts as durable
- **Today:** asynchronous. The shard answers at once, and a recorder thread writes the journal behind it, forcing the disk every 10 ms by default. The matching thread never waits for a disk.
- **Alternative A:** group commit. Hold each report until the sync that covers its command. Nothing a client was told can be lost, at the cost of adding the sync time to every order.
- **Alternative B:** replicate each command to a second machine before answering, and keep the disk asynchronous. This is what most fast venues do.
- **Leaning:** keep asynchronous as the default, and add A as a switch for anyone who wants it. B is a project of its own.
- **Measured so far:** journaling costs the matching thread nothing visible (`shard.rest` against `shard.rest.journal` in `benchmarks.md`). What alternative A would add, the time for a command to reach the disk, is not measured yet.
- **Touches:** W-70, W-80.

### D-13 What the venue does when its journal fails
- **Today:** matching carries on and `planckd` prints an error. The journal stops describing the venue.
- **Alternative:** stop taking orders the moment the journal fails, and reject new ones with a reason.
- **Leaning:** the alternative. A venue that cannot record what it does should not be doing it. Left as it is for now because rejecting needs a path from the recorder back to the gateway.
- **Touches:** W-71.

### D-15 How much of planck the C library covers
- **Today:** the matching engine only. One shard on the caller's thread: submit, cancel, read reports and market events, query the book.
- **Alternative A:** add the journal, so a C or Python program can record a run and replay it.
- **Alternative B:** a whole venue behind the C interface, threads and sockets included, with start and stop calls.
- **Alternative C:** a client library instead: the wire protocol's encode and decode, so other languages can talk to `planckd`.
- **Leaning:** keep the engine as it is, add C when a client in another language is wanted. B has little point, since `planckd` is already a program anyone can run.
- **Touches:** W-90.

### D-16 How the C library hands back what happened
- **Today:** the caller pulls. After each call it reads reports and market events from two queues until they are empty. No callbacks.
- **Alternative:** the caller registers two functions and the engine calls them during the match.
- **For pulling:** nothing calls back into foreign code from the middle of matching, which every binding generator can handle and which cannot re-enter the engine. **For callbacks:** no queues, so W-91's memory goes away.
- **Leaning:** keep pulling as the default. Offer callbacks as a second mode if the memory matters to someone.
- **Touches:** W-91.

### D-17 A C++ wrapper on top of the C library
- **Today:** none. `capi/` is the lower two thirds of the hourglass pattern: a C++ implementation behind a narrow C interface. C++ programs that want the engine link planck's own libraries directly.
- **Alternative:** add the top of the hourglass: one header, `capi/include/planck.hpp`, that wraps `planck.h` in a move-only `Engine` class with real enum types and ranges over reports and market events. Header only, no exceptions, nothing compiled into the library.
- **For it:** a C++ program built with a different compiler or standard library could use the engine safely, which planck's own headers do not allow. **Against it:** nobody needs that yet, and it is a second interface to keep in step with the first.
- **Leaning:** leave it out until something outside this repository wants to use the engine from C++.
- **Touches:** W-90.

### D-18 Letting a client keep its orders working while it is away
- **Today:** no. Every client's resting orders are cancelled when its connection ends (S-27).
- **Alternative:** a client says at login that its orders should stay. That is only safe if a returning client can be told everything that happened to them meanwhile. Within one run of the venue it now can (S-33). Across a restart it cannot (W-98), and a restart cancels everything anyway (S-28).
- **Leaning:** offer it after step 4 of the plan, when held reports survive a restart, and never as the default.
- **Touches:** W-98, W-99.

### D-19 A limit on how many orders one session may have resting
- **Today:** none. One session may fill the whole store, and closing it then cancels all of that in one command, during which the shard serves nobody else.
- **Alternative A:** a limit per session, checked when an order comes to rest. A venue wants this as a risk control anyway.
- **Alternative B:** spread a large close over several passes of the shard, a bounded number of cancels at a time, with other commands in between.
- **Leaning:** A first. It bounds the stall and is useful in its own right. B only if the limit has to be large.
- **Touches:** W-25.

## Settled

| Id | Decision | Why |
|----|----------|-----|
| S-01 | A standalone repository, separate from hotpath and fixcross. | A fresh start aimed only at speed. |
| S-02 | C++23, no exceptions, no RTTI, warnings as errors. NASA Power of Ten rules throughout. | Predictable code on the hot path. |
| S-03 | A flat C interface only at the edge, in `capi/`. C++ everywhere inside. | The "beautiful native libraries" pattern. |
| S-04 | One thread per shard owns its books outright. Threads talk only through single-producer single-consumer rings. | No locks and no shared mutable state on the matching path. |
| S-05 | A full output ring stalls the shard. It never drops a message. | A venue that loses a fill is wrong. One that is briefly slow is only slow. |
| S-06 | Prices are integer ticks in a fixed band per symbol. An order priced outside the band is rejected. | The band makes the price ladder a flat array. Confirmed. |
| S-07 | Cancel is by venue order id only. The id carries the shard, so no lookup is needed to route it. | One fewer table on the hot path. |
| S-08 | Order entry over TCP with fixed-layout little-endian binary messages. Market data over UDP multicast with per-stream sequence numbers. | TCP for reliability where each message matters to one party. UDP for fan-out. |
| S-09 | Wire structs are checked once, at the gateway. Nothing past that point re-validates them. | One trust boundary. |
| S-10 | Classic `#ifndef` include guards. CMake with Ninja. GoogleTest. | House style. |
| S-11 | `apps/` before `journal/`, `perf/`, `capi/` and `bench/`. | A venue that can be started and poked at makes everything after it easier to check. |
| S-12 | The journal records commands, not their effects, and the shard writes it, not the gateway. | Matching is deterministic, so the inputs are enough to rebuild everything, and only the shard knows the order it handled them in and the time it gave each. |
| S-13 | A torn record at the end of a journal is dropped at restart. Damage anywhere else stops the venue from starting. | The first is the normal mark of a crash. The second means history is missing, and a person should decide. |
| S-14 | `bench/` before `capi/`. | Numbers first, so that nothing is frozen behind an interface before its cost is known. |
| S-15 | The benchmarks use their own small harness on top of `perf::Histogram`, not Google Benchmark. | Google Benchmark reports an average time per operation. What matters here is the slow tail of single operations, which needs every sample kept. It is also one dependency fewer. |
| S-16 | A fixed-rate load is measured against the time each order was due, as well as the time it was sent. | Otherwise a load generator slows down whenever the venue does and never sees the queue a real client would sit in (coordinated omission). |
| S-17 | `libplanck.so` compiles the engine's sources a second time, as position independent code. The static libraries the venue links are not touched. | Position independence is needed for a shared library and is not free. The venue's hot path should not pay for a library it does not use. |
| S-18 | `libplanck.so` exports the functions in `planck.h` and nothing else, and needs only the C library at run time. | Internals stay free to change. A C program does not have to load the C++ runtime. Both are checked by a test. |
| S-19 | A caller's mistake (a null pointer, a side that is neither buy nor sell) comes back as a status and changes nothing. The market's refusal (unknown symbol, price outside the band, book full) comes back as a report. | The first is a bug in the calling program. The second is an ordinary event that a trading program has to handle anyway, the same way it would from the venue. |
| S-20 | One build directory for each pairing of compiler and build kind, `build/<compiler>-<kind>`, defined once in `CMakePresets.json`. `build.sh` is a front end to the presets and holds no settings of its own. | Switching compiler or build kind never forces a rebuild, and the script, an editor and a bare `cmake --preset` all land in the same place. |
| S-21 | Five build kinds: release, debug, checked, asan and tsan. "checked" is optimised with contracts on, and is the one tests are meant to run in. | A debug build is too slow for the randomized tests to do much, and a release build assumes its contracts instead of checking them. |
| S-22 | Tools are Python with no packages beyond the standard library, and scripts are bash. | Nothing to install, and they run on any machine that can build planck. |
| S-23 | Who may log in is a file of client ids and tokens, read at startup (`planckd --clients`). Without the file anyone may log in as any client id, which is for tests and a closed lab only. The token travels in the clear, so the link itself must be private: a private network, or a tunnel such as WireGuard. No TLS in the gateway. (Was D-02.) | A table is small and has no moving parts. Encrypting in the gateway would put a cipher on the order path. A tunnel keeps strangers out below the venue, where it costs the matching path nothing. |
| S-24 | A session is not a connection. A session is a client's standing with the venue: its id is the client id, from 1 to 65,535, the same on every connection and across restarts, and it owns the orders. A connection is one TCP link. At most one connection speaks for a session at a time. | Orders must belong to something that survives a dropped line and a restart, or nobody can ever be given their orders or their reports back. Using the client id itself, and not a number looked up from it, means the journal says who owns what with no table to keep in step. It also removes the limit of 65,535 connections in a run. |
| S-25 | A refused login is told why and then disconnected. A client that is not listed and a client with the wrong token get the same answer. | One try per connection, and a stranger cannot learn which client ids exist. |
| S-26 | Journal format 2. A journal written before S-24 is refused at startup. | Its session numbers named connections, not clients, so it cannot say who owns the orders it would rebuild. |
| S-27 | Cancel on disconnect. When a connection that had logged in ends, for any reason, the gateway tells every shard, and each shard cancels what that session has resting. The cancels go to the owner with reason Disconnected and onto the feed as deletes. It is a command like any other: ordered with the session's own commands, journaled, and never dropped. A session cannot log in again until every shard has been sent it. (Was D-01.) | A client that has lost its line cannot manage its orders, and a venue should not trade for someone who is not there. Making it a journaled command keeps replay exact, and ordering it after the session's last order means nothing that session sent can slip past it. |
| S-28 | A venue that starts cancels every resting order. The gateway opens by telling each shard that all sessions are closed, as one journaled command, before it accepts a connection. (Was D-14.) | A restart is a disconnect of everyone. The journal still rebuilds the books first, so the feed shows each order leaving and subscribers stay in step. |
| S-29 | The list of each session's resting orders is kept by the shard, beside the order store, and only in a shard asked to cancel on disconnect (`ShardConfig::cancel_on_disconnect`). `planckd` always asks. The book knows nothing of it. | The order stays at half a cache line and the book stays free of sessions. A shard that is not a venue, in a benchmark or behind the C interface, pays nothing. |
| S-30 | Journal format 3: two more kinds of record, for one session closing and for all of them. Formats 1 and 2 are refused. | A format 2 reader would stop at the new records, and a format 2 journal was written by shards that kept orders, so it would rebuild books that this venue would never have had. |
| S-31 | Silence ends a connection. The venue drops a connection it has heard nothing on for its idle timeout (five seconds by default), and one that has not logged in by then. It tells each client the timeout at login. It sends a heartbeat to any client it has sent nothing else for its heartbeat interval (one second by default). A venue that drops silent clients may not be silent itself. The gateway judges silence by its own clock and only while it is itself running. (Was W-03.) | Without it a line that dies quietly is never noticed, and cancel on disconnect does nothing in exactly the case it is for. Telling the client the timeout means no client has to guess. Giving everyone a fresh start after the gateway was held up means a pause of the venue's own cannot cancel every order in the book. |
| S-32 | Nobody answers a heartbeat. Order entry protocol version 2. | Two ends that both answered would never stop, and one that answers gives a stranger a way to make the venue send. A heartbeat's arrival is the whole of its meaning. Version 1 clients expected an echo, so they are refused. |
| S-33 | Every report to a session is numbered, from 1, across its connections. The numbers are not on the wire: `LoginAccepted` says what the next one is and the client counts. The gateway holds each session's latest reports in memory (`--retain-reports`, 1024) and a client that says at login which number it expects next is sent the rest first. Order entry protocol version 3. | It is the least that lets a client on a bad line know what happened to its orders. Counting costs no bytes and no change to any report, and TCP makes it exact. Holding a fixed number per session keeps memory bounded and off the heap. A client that asks for more than is held is told by where the numbers resume, not refused, because it still needs what there is. |
| S-34 | An order is carried out at most once. A session's client order ids must go up, the gateway remembers the highest, and a `NewOrder` that is not above it is ignored without an answer. An id is used as soon as the gateway reads it. After a restart the highest ids come from the journals. | A client that cannot tell whether an order arrived can send it again and be right either way. The gateway judges, not a shard, because only it sees one session's orders in the order they were sent. No answer to a repeat, because the answer is the original's report, which the client can ask for. One number per session is all it costs. |
| S-35 | Report numbers belong to one run of the venue, named by an epoch that every `LoginAccepted` carries. A restart starts them again at 1 and holds nothing from before. | The numbers are given out by the gateway as reports arrive from the shards, and that interleaving is in no journal. Each shard's journal can rebuild its own reports but not the order a session saw them in, so numbers rebuilt after a restart would not be the ones clients counted. Saying so with an epoch is honest. Recording the gateway's stream is step 4. |
