# Known weaknesses

Things that are wrong, missing, or unproven in planck today. Each entry says
what the problem is, why it matters, and what a fix would look like. When one
is fixed, move it to the "Fixed" list at the bottom with the date. Do not
delete it.

Choices that could reasonably go either way live in `decisions.md`, not here.

Severity: **high** means wrong results or an outage are possible. **medium**
means a real operational gap. **low** means a rough edge.

## Gateway

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-01 | high | Orders left by a disconnected client stay in the book. Their fill reports are dropped. | A client that crashes keeps trading with no way to learn about it. | Depends on D-01. Either cancel on disconnect, or let a client reclaim its orders after reconnecting. |
| W-02 | medium | Session ids are never reused, so a run accepts 65,535 connections in total and then refuses all newcomers. | A long run with reconnecting clients will hit it. | Widen `SessionId` to 32 bits, or reuse ids once no order with that owner remains. |
| W-03 | medium | The gateway never sends heartbeats and never drops an idle client. | A half-open connection holds a session slot forever. A client cannot tell a quiet venue from a dead one. | Per-session last-heard time, a server heartbeat, and an idle timeout. |
| W-04 | medium | Order entry has no sequence numbers and no replay. | Reports sent while a client was disconnected are gone. | Sequence the report stream per session and add a resend request. Needs the journal. |
| W-05 | medium | Login checks only the protocol version and that the client id is not in use. | Anyone who can reach the port can trade as any free client id. | Depends on D-02. |
| W-06 | low | One gateway thread serves every shard and every client. | It becomes the bottleneck before the shards do. | Several gateways, each owning a set of sessions. The rings are single producer, so this needs one command ring per gateway per shard. |
| W-07 | low | A full command ring rejects the order as Busy at once. | A short burst turns into client-visible rejects. | Probably right as is. Revisit with numbers from `bench/`. |

## Feed

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-10 | high | No snapshots. A subscriber that joins late, or loses more than the history holds (65,536 messages per stream by default), cannot rebuild its books. | The only signal is `ReceiverStats::lost` going up. After that the subscriber's books are wrong for the rest of the run. | A snapshot channel: book state as of a sequence number, then the live stream from there. |
| W-11 | high | The retransmit port answers anyone, with no rate limit. A 16-byte request can cause up to 512 messages to be sent to the request's source address. | On an open network that is a traffic amplifier aimed at a spoofed address, and a way to load the publisher thread. | Per-source rate limit, a smaller reply budget, and serving retransmits from a separate thread. |
| W-12 | medium | Recovery is go-back-N. On a gap the receiver discards every newer message until the hole is filled, then asks for them again. | Under steady loss it re-requests far more than it lost. | Buffer out-of-order datagrams and request only the missing ranges. |
| W-13 | medium | Retransmits are served by the same thread that publishes the live stream. | A burst of requests delays live market data. | Same fix as W-11: a separate thread reading the history. |
| W-14 | low | A datagram the kernel refuses to send is tried once more, then counted and dropped. | Subscribers recover through a gap request, which costs them time. | Acceptable for UDP. Watch `PublisherStats::send_failures`. |
| W-15 | low | The receiver gives up after 100 unanswered requests, 2 ms apart, with no backoff. | 200 ms may be too short for a publisher that is restarting. | Make both configurable and back off. |

## Engine and book

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-21 | medium | A shard that cannot hand off its output stalls, and aborts the process after 2^34 spins. | One dead consumer takes the shard down with it. That is deliberate (it never drops a message), but there is no softer path. | Detect a dead consumer sooner and fail over, or shut down cleanly with a clear message. |
| W-22 | low | Limits are compile-time constants: 16 shards, 4096 symbols, 1024 session slots. Order capacity and price bands are fixed at start. | Changing any of them means a rebuild or a restart. | Fine for now. List them in the README. |

## Journal

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-70 | high | A command is answered before it is on disk. If the machine loses power, up to `--journal-sync` milliseconds of commands (10 by default) are gone, although clients were told they happened and the feed published them. | After such a restart the venue's books disagree with what clients and subscribers believe. A plain process crash loses nothing. | Depends on D-12. Hold reports until the sync that covers them, or replicate to a second machine. |
| W-71 | high | If the disk fails or fills, the recorder stops writing and matching carries on. `planckd` prints one line to stderr. | From that moment a restart loses everything after the failure, and nobody may have noticed. | Depends on D-13. |
| W-72 | medium | The journal grows without limit, 48 bytes a command, and a restart replays all of it. There are no snapshots and no rotation. | A long-running venue has a slow restart and an ever larger file. | Snapshot the books at a sequence number, start a new file there, replay only what follows. Shares its design with W-10. |
| W-73 | medium | Restart republishes the whole market data stream from sequence 1. Subscribers that kept running ignore what they have already seen, which works, but only because replay is exact. | A burst of old data at every restart, growing with the journal. | Goes away with snapshots (W-72). |
| W-74 | medium | A journal can only be replayed into the same configuration: shard count, symbols, bands and order capacity. Anything else is refused. | Adding a symbol or raising a limit means throwing the journal away. | A migration tool, or a configuration record inside the journal so the venue can rebuild with the old settings and then change. |
| W-75 | low | One recorder thread writes every shard's file, to one directory, with ordinary buffered writes. | With many busy shards the recorder, not the disk, becomes the limit, and a shard that outruns it stalls. | One recorder per disk, and preallocated files. Measure first. |
| W-76 | low | Session ids restart above the highest one in the journal, so the 65,535 limit (W-02) now spans every restart. | A venue with a long journal runs out sooner. | Fixed by the fix for W-02. |

## Clock

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-30 | medium | On the WSL2 development machine the kernel was slewing `CLOCK_REALTIME` and `CLOCK_MONOTONIC` at +100,000 ppm against `CLOCK_MONOTONIC_RAW`. Which of them is right is not established. | Timestamps and any latency measured on that machine may be off by 10 percent. | Recheck after `wsl --shutdown`. Do not trust latency numbers taken under WSL2 until this is understood. |
| W-31 | low | Between re-anchors (at most once a second, and only when idle) the shard's clock drifts from the wall clock. A shard that is never idle never re-anchors. | Timestamps wander from real time under sustained load. | Re-anchor on a timer as well, from the idle path of a neighbouring thread, or accept and document the bound. |

## Programs

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-60 | medium | `planckd` is configured only by flags. Symbols are numbered 1 to N with no names, and every symbol shares one price band. | A real symbol list needs a band per symbol and a stable mapping from name to id. | A small config file: id, name, band and shard per symbol. See D-10. |
| W-61 | medium | Orders that arrive after the shards have stopped get no answer, and nothing tells connected clients that the venue is going down. | A client sees a silent close in the middle of its work. | Stop accepting first, answer or reject what is queued, then close each session. |
| W-62 | low | `planck-mdtap` prints events and does not rebuild books. There is no top-of-book view. | It cannot show that the feed alone is enough to hold a book. Only the tests prove that. | Add a `--book` mode with a fixed-size order table. |
| W-63 | low | `planck-client` sends one request and waits for its answer before the next. | It cannot generate load. | That is the job of `bench/`. Keep the client simple. |
| W-64 | low | `planckd` counters reach the printout up to 1024 loop passes late, and only when asked with `--stats`. There is no log file and no way to query a running venue. | Little visibility in operation. | A stats socket or a shared-memory page, once `perf/` exists. |
| W-65 | low | `planck-replay` checks and prints a journal. It does not rebuild books from one or compare two runs. | Looking into a journal by hand is the only offline tool. | Add a mode that replays into a shard and prints the resulting books. |
| W-66 | low | The latency `planckd` prints covers request off the socket to report picked up for sending. It leaves out the kernel's network stack on both sides and the final `send()`. | It understates what a client sees. | `bench/` will measure from the client's side. Hardware timestamps for the rest. |

## Not tested

| Id | What has never been exercised |
|----|-------------------------------|
| W-40 | Slow-consumer disconnect in the gateway (`GatewayStats::slow_consumers`). |
| W-41 | Running out of session ids. |
| W-42 | Thread pinning for the gateway and the publisher. |
| W-43 | Huge pages, isolated cores, real-time priority. |
| W-44 | Multicast between two hosts. The test runs within one host and skips itself where multicast is not routed. |
| W-45 | Packet loss, reordering and duplication from a real network. Tests drop whole datagrams by hand. |
| W-46 | The timestamp clamp when the wall clock really steps backwards. |
| W-47 | AArch64. The code has paths for it and none has been compiled. |
| W-48 | The `journal/` and `perf/` modules, and the journaling changes to the shard, gateway and `planckd`, under GCC 16. They were developed with GCC 13 and Clang 18. Whether the full test suite passes on GCC 16 for `net/`, `gateway/` and `feed/` has also not been reported, only that the programs build and trade. |
| W-49 | `planckd` with `--cpus`, `--lock-memory`, `--huge require` or `--idle spin`. The end-to-end test runs unpinned on loopback, and the development sandbox has two CPUs. |
| W-52 | `planck-client` typed at from a terminal. Every run so far fed it a script through a pipe. |
| W-55 | A journal after real power loss. Torn and damaged files are made by hand in the tests. Killing the process is tested, and loses nothing. |
| W-56 | The recorder when the disk fills or fails. The code path that sets `failed()` has never run. |
| W-57 | The journal and recorder with more than two shards, or on a slow disk. |
| W-53 | The three programs talking over multicast inside the automated test. It was done once by hand. The end-to-end test uses plain loopback addresses so it passes on hosts without multicast. |

## Measurement

| Id | Severity | Weakness | Fix |
|----|----------|----------|-----|
| W-50 | high | There are no trustworthy latency numbers. `planckd --stats` now prints percentiles, but nothing has been measured on pinned, isolated cores, and the journal's cost to the shard is unmeasured. The project's one-line claim is nanosecond tail latency. | `bench/`, on real hardware. |
| W-54 | low | The end-to-end test picks its four ports from its process id, in the range 12000 to 28000. Another program already on one of them makes the test fail. | Let the programs bind port 0 and report what they got, then have the script read it. |
| W-51 | medium | The seeded-bug check (plant a bug, confirm a test fails) is a throwaway script outside the repo. An earlier version of it gave false passes. | Put a proper version in `tools/` and run it before each milestone. |

## Fixed

| Id | Date | What was wrong | What changed |
|----|------|----------------|--------------|
| F-01 | 2026-11 | `SpscRing::drain` returned fewer elements than were queued when its cached view was stale but not empty. | It now refreshes whenever the cached count is below the requested maximum. |
| F-02 | 2026-11 | The test meant to cover F-01 never created the stale state, so reverting the fix went unnoticed. | `RingTest.DrainSeesEverythingQueued` rewritten. |
| F-03 | 2026-11 | Clock tests compared two clocks sampled at different instants and failed under WSL2. | Paired sampling (`ClockSample`), and the rate is checked against `CLOCK_MONOTONIC_RAW`. |
| F-04 | 2026-11 | Feed and gateway tests assumed loopback delivery within a few milliseconds. One failed under load. | The feed tests now wait until the tap has seen every message the publisher counted as sent. Heartbeat tests use a 100 ms interval. Gateway tests wait up to several seconds for a disconnect to be noticed. |
| F-05 | 2026-11 | With a plain (not multicast) feed address, the first datagram sent after a period with no listener was silently dropped. The kernel reports the earlier "nobody there" on the next send and does not perform that send. | The publisher retries a failed live send once. `FeedTest.ListenerThatArrivesLateGetsTheVeryNextDatagram` covers it. Found by the end-to-end test under load. |
| F-06 | 2026-11 | Every UDP receiver allowed port reuse. The kernel can give two such sockets the same port, even when each asked for any free one, and one then takes the other's datagrams. Tests running side by side lost whole streams this way, and so could a subscriber's retransmit replies. | Only multicast receivers allow reuse now. A plain address and port can be bound once. `Udp.APlainPortBelongsToOneSocket` covers it. Found by repeated parallel test runs. |
| F-07 | 2026-11 | Nothing was persisted. A restart lost every resting order. (Was W-20.) | `journal/`: every command is recorded, and `planckd --journal` rebuilds the books and the market data stream from it at startup. |
| F-08 | 2026-11 | After a restart a new session could have been given the id of an old one and inherited its resting orders. | The gateway starts its session ids above the highest in the journal. |
| F-09 | 2026-11 | GCC 16 had not built `net/`, `gateway/`, `feed/` or `apps/`. (Was part of W-48.) | Built and run by hand on GCC 16.0.1 under WSL2: two clients traded through `planckd` and the counters matched. |
