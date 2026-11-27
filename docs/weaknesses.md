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
| W-01 | high | Orders left by a disconnected client stay in the book and go on trading. The client gets them back when it logs in again, but fills that happened meanwhile are never reported to it. | A client that crashes keeps trading, and learns of it late and incompletely. | Decided (D-01): cancel on disconnect. Step 2 of the plan in `decisions.md`. |
| W-03 | medium | The gateway never sends heartbeats and never drops an idle client. | A half-open connection holds a session slot forever. A client cannot tell a quiet venue from a dead one. | Per-session last-heard time, a server heartbeat, and an idle timeout. |
| W-04 | medium | Order entry has no sequence numbers and no replay. | Reports sent while a client was disconnected are gone. | Sequence the report stream per session and add a resend request. Needs the journal. |
| W-05 | medium | Started without `--clients`, the venue lets anyone log in as any client id that is not connected, and so cancel that client's resting orders and receive its fills. | An open venue is safe only where every machine that can reach the port is trusted. | Use `--clients` anywhere else. Consider refusing to listen on anything but loopback without it. |
| W-08 | medium | The login token is a 64-bit number sent in the clear, and the same one every time. Anyone who can read the traffic can log in as that client from then on. Failed logins are counted but not limited. | The token keeps clients apart from each other. It does not keep out anyone who can see or alter the traffic. | Run order entry only over a private link (S-23). A challenge and response would stop a captured token being used again. Limit failed logins per source address. |
| W-09 | low | The clients file is read once, at startup. It holds at most 4,096 clients, and its tokens are stored as written. | Adding a client or changing a token means a restart. Anyone who can read the file can log in as anyone. | Reload on a signal. Keep the file readable by the venue's user only. |
| W-06 | low | One gateway thread serves every shard and every client. | It becomes the bottleneck before the shards do. | Several gateways, each owning a set of sessions. The rings are single producer, so this needs one command ring per gateway per shard. |
| W-07 | low | A full command ring rejects the order as Busy at once. | A short burst turns into client-visible rejects. | Probably right as is. `planck-loadgen` counts Busy rejects, so drive it at a fixed rate on real hardware and see how soon they start. |

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
| W-22 | low | Limits are compile-time constants: 16 shards, 4096 symbols, 1024 session slots. Order capacity and price bands are fixed at start. | Changing any of them means a rebuild or a restart. | Fine for now. They are listed under "Limits" in `architecture.md`. |
| W-23 | low | An order that may rest is given its slot before it is matched. When every slot is taken it is rejected as BookFull, even if it would have traded at once and freed more room than it needed. Orders that never rest (IOC, FOK, market) are not affected. | At the very limit of capacity the book turns away liquidity it could have matched. | Match first when the order crosses, and take a slot only for what is left. That needs care: fills cannot be taken back if no slot turns up afterwards. Or keep a few slots in reserve for this case. |
| W-24 | medium | A whole command through a shard takes about 85 ns at the median in the sandbox. The book's own part of that is about 16 ns. The other 70 ns is the ring hop, the clock read, encoding the replies, staging them and pushing them out, and none of it has been profiled. | Most of the time on the matching path is spent around the matching, where there may be easy gains. | Profile `planck-bench --filter shard.inline` with `perf`. First suspects: two wire messages built and copied per order, and stage-then-copy for output that could go straight to the ring. |

## Journal

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-70 | high | A command is answered before it is on disk. If the machine loses power, up to `--journal-sync` milliseconds of commands (10 by default) are gone, although clients were told they happened and the feed published them. | After such a restart the venue's books disagree with what clients and subscribers believe. A plain process crash loses nothing. | Depends on D-12. Hold reports until the sync that covers them, or replicate to a second machine. |
| W-71 | high | If the disk fails or fills, the recorder stops writing and matching carries on. `planckd` prints one line to stderr. | From that moment a restart loses everything after the failure, and nobody may have noticed. | Depends on D-13. |
| W-72 | medium | The journal grows without limit, 48 bytes a command, and a restart replays all of it. There are no snapshots and no rotation. | A long-running venue has a slow restart and an ever larger file. | Snapshot the books at a sequence number, start a new file there, replay only what follows. Shares its design with W-10. |
| W-73 | medium | Restart republishes the whole market data stream from sequence 1. Subscribers that kept running ignore what they have already seen, which works, but only because replay is exact. | A burst of old data at every restart, growing with the journal. | Goes away with snapshots (W-72). |
| W-74 | medium | A journal can only be replayed into the same configuration: shard count, symbols, bands and order capacity. Anything else is refused. | Adding a symbol or raising a limit means throwing the journal away. | A migration tool, or a configuration record inside the journal so the venue can rebuild with the old settings and then change. |
| W-75 | low | One recorder thread writes every shard's file, to one directory, with ordinary buffered writes. | With many busy shards the recorder, not the disk, becomes the limit, and a shard that outruns it stalls. | One recorder per disk, and preallocated files. Measure first. |

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
| W-63 | low | `planck-client` sends one request and waits for its answer before the next. | It cannot generate load. | That is `planck-loadgen`'s job. Keep the client simple. |
| W-85 | low | `planck-loadgen` sends no token, so it cannot log in to a venue started with `--clients`. Its client ids must also stay at or below 65,535, which it does not check. `planck-client` takes its token on the command line, where other users of the machine can see it. | A venue under load test has to be an open one. | Give the generator a clients file of its own. Let the client read its token from a file or the environment. |
| W-64 | low | `planckd` counters reach the printout up to 1024 loop passes late, and only when asked with `--stats`. There is no log file and no way to query a running venue. | Little visibility in operation. | A stats socket or a shared-memory page. |
| W-65 | low | `planck-replay` checks and prints a journal. It does not rebuild books from one or compare two runs. | Looking into a journal by hand is the only offline tool. | Add a mode that replays into a shard and prints the resulting books. |
| W-66 | low | The latency `planckd` prints covers request off the socket to report picked up for sending. It leaves out the kernel's network stack on both sides and the final `send()`. | It understates what a client sees. | `planck-loadgen` measures from the client's side, kernel included. Hardware timestamps for the rest. |

## Not tested

| Id | What has never been exercised |
|----|-------------------------------|
| W-40 | Slow-consumer disconnect in the gateway (`GatewayStats::slow_consumers`). |
| W-42 | Thread pinning for the gateway and the publisher. |
| W-43 | Huge pages, isolated cores, real-time priority. |
| W-44 | Multicast between two hosts. The test runs within one host and skips itself where multicast is not routed. |
| W-45 | Packet loss, reordering and duplication from a real network. Tests drop whole datagrams by hand. |
| W-46 | The timestamp clamp when the wall clock really steps backwards. |
| W-47 | AArch64. The code has paths for it and none has been compiled. |
| W-48 | Step 1 of the plan (sessions and the clients file) under GCC 16. It was developed with GCC 13 and Clang 18. Everything before it has passed its 300 tests on GCC 16.0.1 under WSL2. |
| W-86 | A venue reached through a WireGuard tunnel, or over any link that is not loopback. S-23 depends on it and nothing here has tried it. |
| W-68 | `build.sh --cxx` and `--cc` with a compiler under another name, `scripts/bench.sh` with any pinning option, and `scripts/check_host.sh` on a machine that is in fact tuned. On the sandbox it can only ever warn. |
| W-77 | The `clang-asan` and `clang-tsan` configurations. The sandbox has Clang 18 without its sanitizer runtime, so neither has ever linked. The sanitizers have only been run with GCC. `scripts/check_all.sh` leaves these two out unless asked with `--full`. |
| W-69 | Clang 19 or later. The release build now uses `__builtin_assume` on every Clang version, tried only on 18. |
| W-49 | `planckd` with `--cpus`, `--lock-memory`, `--huge require` or `--idle spin`. The end-to-end test runs unpinned on loopback, and the development sandbox has two CPUs. |
| W-52 | `planck-client` typed at from a terminal. Every run so far fed it a script through a pipe. |
| W-55 | A journal after real power loss. Torn and damaged files are made by hand in the tests. Killing the process is tested, and loses nothing. |
| W-56 | The recorder when the disk fills or fails. The code path that sets `failed()` has never run. |
| W-57 | The journal and recorder with more than two shards, or on a slow disk. |
| W-58 | `libplanck.so` from any language other than C, C++ and Python. From Python only through cffi in ABI mode, and that test is skipped where cffi is not installed and in sanitizer builds. |
| W-59 | `planck-loadgen` with more than four sessions, for longer than a few seconds, or against a venue on another machine. `planck-bench` with `--cpu` and `--cpu2` on isolated cores. |
| W-67 | Build options in combination. Two were tried besides the default: apps and C library off, and tests off. |
| W-53 | The three programs talking over multicast inside the automated test. It was done once by hand. The end-to-end test uses plain loopback addresses so it passes on hosts without multicast. |

## Measurement

| Id | Severity | Weakness | Fix |
|----|----------|----------|-----|
| W-50 | high | There are no trustworthy latency numbers. The tools now exist (`planck-bench`, `planck-loadgen`, see `benchmarks.md`), but every figure so far comes from a shared virtual machine with two CPUs. Nothing has been measured on pinned, isolated cores. The project's one-line claim is nanosecond tail latency. | Run both on real hardware with `--cpu`, isolated cores and a Release build, and write the results into `benchmarks.md`. Not under WSL2 until W-30 is understood. |
| W-80 | medium | Nothing measures how long a command takes to reach the disk, how fast the recorder can write, or what the gateway and the publisher cost on their own. `planck-bench` shows only what journaling costs the matching thread, which is nothing visible. | D-12 needs the time to durable, and that is still unmeasured. Add a recorder benchmark, and report sync latency from `planckd`. |
| W-81 | low | `planck-loadgen` times new orders only. Cancels are sent and counted but not timed. It is one thread, so above some rate the generator is the bottleneck. The due-time row shows that lateness, but nothing says whose fault it was. | Time cancels too. Report how late orders were sent, separately from how long the venue took. |
| W-82 | low | `scripts/bench.sh` keeps each run and `tools/bench_compare.py` flags what got slower, but only when someone runs them. Nothing does so on a schedule, and no reference results from a quiet machine are kept to compare against. | Once there is hardware to measure on (W-50), keep its results in the repository and compare against them before each milestone. |
| W-83 | low | The stopwatch takes its own smallest cost off every sample, about 20 ns in the sandbox. For operations of a few nanoseconds, such as one ring push and pop, the result is the difference of two similar numbers and is not precise. Histogram buckets add up to 3 percent. | Treat rows under about 10 ns as "too small to time singly". Time those in batches instead. |
| W-54 | low | The end-to-end tests pick their ports from their process id: `apps_smoke` in 12000 to 28000, `bench_smoke` in 28000 to 40000. Another program already on one of them makes the test fail. | Let the programs bind port 0 and report what they got, then have the scripts read it. |
| W-51 | low | `tools/seeded_bugs.py` plants 56 bugs. 39 of them are in `capi/` and `bench/` and 12 are from the session work in `gateway/`. The bugs planted by hand while the earlier modules were written (`book/`, `ring/`, `engine/`, `net/`, `feed/`, `journal/`, `perf/` and the rest of `gateway/`) were never written down and are not in the catalogue. | The older modules are only as well guarded as the day they were written. Add a catalogue entry with every fix and every risky line from now on, and backfill the old modules a few at a time. |
| W-84 | low | Nothing runs the checks automatically. `scripts/check_all.sh` and `tools/seeded_bugs.py` do the work, but someone has to start them. A full seeded-bug run takes about 20 minutes on two CPUs. | A change can be made without anyone running the sanitizers. Set up CI when the repository has a home, with the quick matrix on every change and the full one nightly. |

## C interface

| Id | Severity | Weakness | Why it matters | Fix |
|----|----------|----------|----------------|-----|
| W-90 | medium | `libplanck.so` is the matching engine and nothing else: one shard, driven on the caller's thread. No journal, no replay, no gateway, no feed, no second shard. | Enough to embed the books in a simulator or a backtest. Not enough to run or talk to a venue from another language. | Depends on D-15. |
| W-91 | medium | The engine's output queues are sized so that one order sweeping a full book cannot overflow them, because on a single thread nothing can drain them mid-match. That is up to about 400 bytes for every order of capacity: about 25 MB at the default 65,536 and about 870 MB at the 4,000,000 limit. | Most of that memory exists for a case that almost never happens. | Let a shard that is driven inline hand each message straight to the caller instead of queueing it. Depends on D-16. |
| W-92 | low | Every timestamp comes from the engine's own clock. A caller cannot supply the time. | Two runs of the same orders give the same books but different timestamps, so a backtest is not reproducible byte for byte. The shard can already take a forced time, for replay. It is not exposed. | A config flag for caller-supplied time, and a timestamp argument on submit and cancel. |
| W-93 | low | `planck_engine_create` blocks for about 50 ms while the engine calibrates its clock, and every engine does so separately. | Creating many engines, as a parameter sweep would, is slow. | Calibrate once per process and share the result. |
| W-94 | low | No install rules, no pkg-config file, no static variant of the library, and no language binding beyond the header. Linux only: the build relies on a linker version script. | Using the library outside this build tree means copying two files by hand. | Install rules and a `planck.pc`, when `scripts/` and packaging are done. |
| W-95 | low | The promise that no call aborts on bad input rests on the checks in `capi/src/planck.cpp` being complete. Past them, a broken contract still aborts the host process in a checked build and is undefined behaviour in a release build. | A missed check at the boundary would take down the program that loaded the library. | Fuzz the C interface. Every struct is small and fixed, so this is cheap. |

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
| F-10 | 2026-11 | The test suite had not been run on GCC 16 past `engine/`. (Was part of W-48.) | All 268 tests of the modules up to and including `journal/` and `perf/` pass on GCC 16.0.1 under WSL2. |
| F-11 | 2026-11 | The journal's cost to the matching thread was unmeasured. (Was part of W-50.) | `planck-bench` times a shard with and without journaling. The difference is within the noise of the sandbox: about 85 ns either way at the median. |
| F-12 | 2026-11 | A release build with Clang 18 did not compile at all: contracts in "assume" mode demanded `[[assume]]`, which Clang has only from version 19. Every earlier Clang run had been a checked build, so nobody noticed. | Clang now uses `__builtin_assume`, which every version has. Found the first time `build.sh --clang --release` was run. `scripts/check_all.sh` builds that configuration every time. |
| F-13 | 2026-11 | Built with Clang as the C compiler, `libplanck.so` listed the C++ runtime as a dependency although it used nothing from it. GCC's driver drops unused libraries by default and Clang's does not. | The library is linked with `--as-needed`. Caught by `capi_exports` the first time the whole build used Clang. |
| F-14 | 2026-11 | The seeded-bug check was a throwaway script outside the repository. One version gave false passes, and the next left a planted bug's binaries behind. (Was W-51.) | `tools/seeded_bugs.py` works in a private copy, checks that the tests pass before it changes anything, and restores each file before the next build. The catalogue is checked against the source by `tools_selftest` on every test run. |
| F-15 | 2026-11 | Benchmark results were printed and forgotten. (Was W-82.) | `scripts/bench.sh` saves each run with a note of the machine and compiler, and `tools/bench_compare.py` compares two runs. |
| F-16 | 2026-11 | `BenchStopwatch` slept 20 ms and demanded that the stopwatch report at least 20 ms. The sleep runs on one kernel clock and the stopwatch is calibrated against another, and under WSL2 the two run at different rates (W-30). It failed on the first run under GCC 16. | The test compares the stopwatch with the clock it is calibrated against, read around the same sleep. |
| F-17 | 2026-11 | `tools/check_style.py` skipped only a directory called `build`, so a build tree under any other name was checked as if it were source, and failed. | It skips any directory that holds a `CMakeCache.txt`, and any hidden directory. |
| F-18 | 2026-11 | `bench/`, `capi/`, `tools/`, `scripts/` and `build.sh` had not been built with GCC 16. (Was W-48.) | All 300 tests pass on GCC 16.0.1 under WSL2, after F-16 and F-17. |
| F-19 | 2026-11 | Session ids were never used again, so a run accepted 65,535 connections in total and then refused everyone, and with a journal the limit spanned every restart. (Was W-02, W-41 and W-76.) | A session id is now the client id (S-24). Connections come and go without limit. `GatewayTest.ConnectionsCanComeAndGoWithoutLimit` covers it. |
| F-20 | 2026-11 | Login checked nothing but the protocol version. (Was the first form of W-05, and D-02.) | `planckd --clients FILE` lists who may log in and with what token (S-23). What that still leaves open is W-05, W-08 and W-09. |
| F-21 | 2026-11 | After a restart, the orders the journal rebuilt belonged to nobody who could ever log in again. | They belong to the same client ids as before (S-24). The end-to-end test restarts the venue and has the maker hear its old order trade. |
