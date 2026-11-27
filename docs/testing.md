# Testing

354 tests. This page says how to run them, what each group proves, and what
is expected of new code.

## Running

```bash
./build.sh test --checked                 # everything, the usual way
./build.sh test --checked -L unit         # one group
./build.sh test --checked -R OrderBook    # tests whose name matches
./build.sh test --tsan -L integration     # the threaded tests under ThreadSanitizer
scripts/check_all.sh                      # six configurations, one after another
```

Straight ctest works too:

```bash
ctest --test-dir build/gcc-checked -j 8 --output-on-failure
ctest --test-dir build/gcc-checked -R FeedTest --repeat until-fail:20 -j 12
```

The last line is how timing bugs are hunted: many repeats, more jobs than
CPUs. Several real bugs were found exactly that way (F-04, F-05, F-06 in
[weaknesses.md](weaknesses.md)).

To run a test program directly, for a debugger or for gtest's own flags:

```bash
build/gcc-debug/tests/planck_unit_tests --gtest_filter='ShardTest.*'
```

## The groups

Each test has a label. `-L` selects by it.

| Label | Count | What it is | Programs |
|-------|------:|------------|----------|
| `unit` | 226 | One module at a time, deterministic, fast | `planck_unit_tests`, `planck_apps_tests`, `planck_bench_tests` |
| `property` | 23 | Random runs against an independent model | `planck_property_tests` |
| `integration` | 79 | More than one thread, or real sockets on loopback | `planck_integration_tests` |
| `capi` | 21 | The C library, from C++, C and Python | `planck_capi_tests`, `planck_capi_smoke`, two scripts |
| `e2e` | 2 | The programs as separate processes | `apps_smoke.sh`, `bench_smoke.sh` |
| `tools` | 3 | The scripts and the house rules | `scripts_smoke.sh`, `check_style.py`, `test_tools.py` |

Sanitizer builds run 353, because the Python test of the C library is
skipped there. So does a machine without Python's `cffi`.

### unit (`tests/unit/`)

| File | Covers |
|------|--------|
| `core_test.cpp` | Strong types, status names, enum validation |
| `sys_test.cpp` | The clock against the kernel's, CPU list parsing, pinning, memory mapping |
| `mem_test.cpp` | Arena, pool (including reuse order), cache-line alignment |
| `ring_test.cpp` | Push, pop, batch push, drain, full and empty, layout |
| `ladder_bitmap_test.cpp` | Finding the next occupied price level |
| `order_book_test.cpp` | Every matching rule: priority, partial fills, IOC, FOK, market orders, bands, capacity, cancels |
| `wire_test.cpp` | Every message: size, round trip, truncation, bad type, reserved bytes, plus golden bytes that pin the format |
| `shard_test.cpp` | A shard driven by hand: reports, market events, stalls, journaling, replay, sessions closing |
| `net_test.cpp` | Address parsing, TCP and UDP sockets, the poller |
| `perf_test.cpp` | Histogram buckets and percentiles |
| `journal_test.cpp` | File format, reader, writer, torn tails, damage in the middle |
| `credentials_test.cpp` | The clients file: who is permitted, every kind of bad line, limits |
| `apps_support_test.cpp` | Argument parsing and formatting for the programs |
| `bench_test.cpp` | Pacer, send log, stopwatch |
| `capi_test.cpp` | The C interface, including a random run compared with a shard driven directly |

### property (`tests/property/`)

These generate thousands of random commands from a fixed seed and check
every result against something that shares no code with the engine.

| File | Checks the engine against |
|------|---------------------------|
| `book_differential_test.cpp` | `ReferenceBook`: a book written to be obviously correct, with ordered maps and linear scans |
| `shard_audit_test.cpp` | `FeedAuditor`: books rebuilt from market data alone, and each order's state as told to its owner. The two must agree with each other and with the shard. |
| `journal_replay_test.cpp` | Itself: a run is stopped, rebuilt from its journal and continued. Books and the feed must come out exactly as before. Also torn tails, damage in the middle, wrong configurations and sync timing. |

Set `PLANCK_TEST_SCALE=20` in the environment to run twenty times as many
commands. Do that before a release and after touching `book/` or `engine/`:
it takes about three minutes, and it is what found a close that could wait
for ever on a single thread, which no shorter run had hit.

### integration (`tests/integration/`)

| File | Covers |
|------|--------|
| `ring_stress_test.cpp` | Two threads, millions of messages, every one delivered once and in order, for several ring sizes |
| `shard_thread_test.cpp` | A shard on its own thread through a long random run, with a `FeedAuditor` checking every message |
| `gateway_test.cpp` | Login and who is let in, sessions that outlive connections, cancel on disconnect, heartbeats and silence, numbered reports and their recovery, orders sent twice, routing, rejects, protocol violations, connection limits, all over real TCP |
| `feed_test.cpp` | Sequencing, batching, heartbeats, gaps, retransmission, giving up, and a book rebuilt across a real socket |
| `full_stack_test.cpp` | Shard, gateway and publisher threads together, with clients on TCP and a subscriber on UDP |
| `journal_thread_test.cpp` | Shard on one thread and recorder on another: every command reaches the file, in order |

### e2e (`tests/e2e/`)

`apps_smoke.sh` starts `planckd`, trades through it with `planck-client`,
watches the feed with `planck-mdtap`, kills the venue with `kill -9`,
restarts it from the journal and checks that the feed carried over and that
the orders left by the crash were cancelled. It also checks that a client
that leaves has its orders cancelled. The venue runs with a clients file,
and the test checks that a wrong token and an unlisted client are turned
away. Last, it opens a connection by hand, logs in and rests an order byte
by byte, and then says nothing: the venue must drop it and cancel the
order, while a real client that is merely quiet stays.
`bench_smoke.sh` runs both benchmark programs and
checks that their figures add up.

## Test support (`tests/support/`)

| File | Gives you |
|------|-----------|
| `reference_book.hpp` | `ReferenceBook`, the slow and obviously correct order book |
| `feed_auditor.hpp` | `FeedAuditor`, and `RandomFlow`, which produces plausible and implausible commands, sessions closing among them |
| `shard_harness.hpp` | `small_shard()`, `new_order()`, `cancel_order()`, `session_closed()`, and `as<T>()` to decode what a shard emitted |
| `wire_client.hpp` | `WireClient`, a minimal order entry client over TCP that never blocks. It sends no heartbeats, so tests that want a timeout say so. It counts reports as the protocol tells a client to: `resume()` logs in asking for missed ones, `next_report()` is the count. |
| `fixtures.hpp` | `TradeLog`, and `scaled()` for `PLANCK_TEST_SCALE` |
| `temp_dir.hpp` | `TempDir`, a directory removed when the test ends |

## Seeded bugs

A passing test suite proves little unless it would fail when the code is
wrong. `tools/seeded_bugs.py` checks that directly: it plants one small bug
at a time in a private copy of the repository, rebuilds, and confirms that
a test fails.

```bash
tools/seeded_bugs.py                # all 132, about 55 minutes on two CPUs
tools/seeded_bugs.py --only capi    # the ones whose label matches
tools/seeded_bugs.py --list
```

The bugs are in `tools/seeded_bugs_catalogue.py`. A bug that survives
means a test is missing. This has found real holes every time it has been
run: seven of the 41 bugs first planted in `capi/` and `bench/` survived
until tests were added.

**Run all of it after a change of any size, not only the entries near the
change.** A change in one place can make a test somewhere else unable to
fail. Cancel on disconnect did that to a check in the bench smoke test,
and it went unnoticed for two steps because only the nearby bugs were
rerun (F-31).

**Add a catalogue entry whenever you fix a bug or write a line that would
be easy to get subtly wrong.** The entry is the change that would bring
the bug back. `tools_selftest` checks on every test run that each entry's
pattern still occurs exactly once in the source, so the catalogue cannot
silently rot.

The catalogue is lopsided: 39 of its 132 entries are in `capi/` and `bench/`,
12 are from the session work in `gateway/`, 24 are for cancel on disconnect
15 for heartbeats and silence and 37 for report recovery and repeated
orders (W-51). Backfilling the older modules is open work.

## What is expected of new code

1. **A test for every rule.** If a comment says "X never happens" or "Y is
   rejected", a test makes X or Y happen.
2. **Test names say what must be true**, as a sentence:
   `NobodyElseCanCancel`, `ReplayWaitsForRoomAndLosesNothing`. Failure
   messages say what went wrong in the system's terms, not the test's.
3. **Assert on counts before contents.** Check how many messages came out
   before looking inside them.
4. **No sleeps to wait for something.** Poll for the condition with a
   generous limit. A fixed sleep is either too short on a busy machine or
   too long everywhere. See `eventually()` in `net_test.cpp` and
   `captured()` in `feed_test.cpp`. And where the code under test measures
   time, give it a clock the test controls: see `GatewayTimers` in
   `gateway_test.cpp`, which tests timeouts of half a second in no time at
   all.
5. **Pass in all of `scripts/check_all.sh`**, and survive
   `--repeat until-fail:10 -j 12` if threads or sockets are involved.
6. **A seeded bug** for anything subtle.

## What is not tested

The full list is the "Not tested" table in [weaknesses.md](weaknesses.md).
The ones most likely to matter: pinned threads, huge pages and isolated
cores (W-43, W-49), multicast between hosts (W-44), real packet loss
(W-45), real power loss (W-55), a failing disk (W-56), GCC 16 for the
newest directories (W-48), and the Clang sanitizer builds (W-77).
