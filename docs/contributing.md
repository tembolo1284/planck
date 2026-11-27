# Contributing and taking over

How to change planck without breaking what makes it fast or what makes it
checkable, and what to know when you take over a module.

## The workflow

The project has been built one module at a time, in the same loop each
time. Keep it.

1. **Write the module with its tests.** Not after.
2. **Plant bugs.** Add entries to `tools/seeded_bugs_catalogue.py` for the
   lines that would be easy to get subtly wrong, and run
   `tools/seeded_bugs.py --only <label>`. A survivor means a missing test.
3. **Run the matrix.** `scripts/check_all.sh`. Threads or sockets involved:
   also `ctest --repeat until-fail:10 -j 12` on those tests.
4. **Circle back.** Write down what was verified and what was not. New
   weaknesses go in `weaknesses.md`. Choices that could have gone another
   way go in `decisions.md`. Fixed weaknesses move to the Fixed table with
   what changed. They are never deleted.

Step 4 is the one that gets skipped, and it is why a newcomer can trust the
docs. A change that makes `weaknesses.md` longer is a good change if the
weakness was already there.

## Rules for the code

These follow the NASA "Power of Ten" rules, adapted to C++ (S-02).

**Language**

- C++23. No exceptions, no RTTI. Both are turned off by the compiler.
- Warnings are errors, with `-Wconversion`, `-Wsign-conversion` and
  `-Wshadow` on. Narrowing is always written out.
- Classic `#ifndef PLANCK_<MODULE>_<FILE>_HPP` include guards. Not
  `#pragma once`.
- Fallible functions return `core::Status`, which is `[[nodiscard]]`.
  Functions that cannot fail are `noexcept`.
- Strong types (`core::Price`, `core::Qty`, `core::OrderId` and so on) in
  every interface. Raw integers only at the wire and inside a module.

**Hot path** (anything a shard, the gateway, the publisher or the recorder
runs per message)

- No allocation. Memory comes from the arena, at `init()`.
- No system calls on the shard. The others make only the socket and file
  calls that are their job, never blocking.
- No locks. No atomics except inside `SpscRing`.
- No virtual calls, no `std::function`. Callbacks are template parameters
  so they inline.
- Every loop has a bound that can be read off the code.
- No recursion.

**Contracts**

- `PLANCK_EXPECT` for what the caller must guarantee, `PLANCK_ENSURE` for
  what the function guarantees, `PLANCK_ASSERT` for what must hold in
  between. Use them freely. In release builds they cost nothing and help
  the optimiser.
- A contract is for a bug in this program. Never use one for input from
  outside: bytes off a socket, a file's contents, a command-line flag, an
  argument to the C interface. Those get checked and answered with a
  `Status` or a reject.
- `PLANCK_VERIFY` is checked in every build mode. Cold paths only.

**Layout**

- Structs that cross a thread, a wire or a disk have their size and offsets
  pinned by `static_assert`, and
  `std::has_unique_object_representations_v` to prove there is no padding.
- Data written by different threads never shares a cache line. Use
  `mem::CacheAligned`.

**Comments** say why, and what must stay true. Not what the next line does.

## Rules for the prose

Docs and comments are checked by `tools/check_style.py`, which runs as the
`style` test: no tabs, no trailing spaces, a newline at the end of every
file, and no long dashes. Write short sentences and go easy on semicolons.

## Where things go

| You are adding | Put it in |
|----------------|-----------|
| A type every module needs | `core/` |
| Anything that calls the kernel | `sys/` or `net/` |
| A wire message | `wire/`, plus [protocol.md](protocol.md), plus golden bytes in `wire_test.cpp` |
| A command a shard can execute | `engine/messages.hpp`, `Shard`, the journal's `Record`, `RandomFlow` and `FeedAuditor` in the tests |
| A program | `apps/`, with its argument parsing in `apps/support.*` so it can be unit tested |
| A benchmark | `bench/planck_bench.cpp`, one function and one line in the table |
| Something for other languages | `capi/`, following [c_api.md](c_api.md) |

Dependencies point one way: core, sys, mem, book and ring, wire, engine,
then net, gateway, feed and journal side by side. If a change needs an
arrow pointing back, the design is wrong. Say so before writing it.

## Before you call it done

```bash
scripts/check_all.sh                      # six configurations
tools/seeded_bugs.py --only <your area>   # your planted bugs are caught
python3 tools/check_style.py              # house rules
```

and, if you touched anything on the matching path:

```bash
scripts/bench.sh                          # compares with the previous run
```

Sandbox numbers are noisy. A change in a p50 of less than about 10 percent
means nothing there. See [benchmarks.md](benchmarks.md).

## Taking over a module

For each area: what to read first, what it promises the rest of the system,
where it is weakest, and what is next. Ids point into
[weaknesses.md](weaknesses.md) and [decisions.md](decisions.md).

### book

- **Read:** `order_book.hpp` top to bottom, then
  `tests/support/reference_book.hpp`, which is the same rules written
  slowly.
- **Promises:** price-time priority, trades at the resting price, FOK
  checked before anything trades, O(1) add and cancel, no allocation.
- **Safety net:** `book_differential_test.cpp`. Any change must keep it
  green at `PLANCK_TEST_SCALE=20`.
- **Weakest:** W-23 (a full book rejects an order that would have traded).
- **Open:** D-06 self-trade prevention, D-07 reduce-quantity. Both land
  here first.

### ring

- **Read:** `spsc.hpp`. It is one file and the comments carry the argument
  for why it is correct.
- **Promises:** wait-free, one producer and one consumer, nothing lost or
  duplicated or reordered.
- **Safety net:** `ring_stress_test.cpp` under `--tsan`. F-01 and F-02 are
  the history to read before touching the cached indices.
- **Do not** add a second producer. The gateway design depends on these
  being single producer, and W-06 explains what scaling out needs.

### engine

- **Read:** `messages.hpp`, then `Shard::poll` and `publish` in `shard.cpp`.
- **Promises:** never drops, never blocks except in the one documented
  case, one timestamp per command, deterministic given commands and
  timestamps. The journal depends on that last one completely.
- **Safety net:** `shard_audit_test.cpp` and `journal_replay_test.cpp`.
- **Weakest:** W-24. About 70 of the 85 ns a command takes are spent around
  the book and have never been profiled. This is the next piece of
  performance work. Also W-21 (a dead consumer aborts the process).
- **Rule:** anything that makes matching depend on something other than
  the command and its timestamp breaks replay. No wall-clock reads, no
  randomness, no dependence on ring fill levels.

### wire

- **Read:** [protocol.md](protocol.md), then `codec.hpp`.
- **Promises:** `well_formed` is the only validation anyone does (S-09).
- **Rule:** a new field or enum value needs its check in `well_formed`
  first. Everything behind the gateway trusts it blindly.

### net and gateway

- **Read:** `gateway.hpp`'s header comment, then `Gateway::poll`.
- **Promises:** validates everything, never blocks, never lets one client
  stall another.
- **Weakest:** this is where most product decisions are open. D-01 cancel
  on disconnect, D-02 authentication, W-03 heartbeats and idle timeout,
  W-04 report replay, W-61 clean shutdown. D-01 is the one to settle first,
  since D-14 and W-01 hang on it.
- **Never tested:** slow-consumer disconnect (W-40), running out of session
  ids (W-41).

### feed

- **Read:** `publisher.hpp`, `receiver.hpp`, then `feed_auditor.hpp`.
- **Promises:** per-stream sequence numbers with no gaps, messages never
  split across datagrams, receiver delivers in order exactly once.
- **Weakest:** W-10 no snapshots, W-11 an open retransmit port. Both high.
- **Next:** snapshots (D-05), designed together with journal snapshots
  (W-72), because they are the same thing.

### journal

- **Read:** `format.hpp`, then `replay.hpp`, then the startup sequence in
  `apps/planckd.cpp`.
- **Promises:** every command recorded in the order the shard handled it,
  a torn tail tolerated, damage refused, replay exact to the byte.
- **Weakest:** W-70 asynchronous durability, W-71 a failed disk does not
  stop the venue. Both high, both waiting on decisions (D-12, D-13).
- **Never tested:** real power loss (W-55), a failing disk (W-56).

### apps

- **Read:** `planckd.cpp` from `main` down. Startup order matters and is
  explained in [architecture.md](architecture.md).
- **Safety net:** `tests/e2e/apps_smoke.sh`.
- **Next:** a config file for symbols (D-10, W-60).

### bench

- **Read:** [benchmarks.md](benchmarks.md), then `harness.hpp`.
- **Weakest:** W-50. The tools work and no number from them can be trusted
  yet. Running them on real, isolated hardware is the single most valuable
  thing anyone can do for this project.

### capi

- **Read:** [c_api.md](c_api.md), then `planck.h`.
- **Rule:** no libstdc++ at run time, nothing exported but `planck_*`.
- **Open:** D-15 scope, D-16 callbacks, D-17 a C++ wrapper header.

### tools, scripts, build

- **Read:** `./build.sh help`, then [building.md](building.md).
- **Rule:** tools are Python with the standard library only, and scripts
  are bash (S-22). `build.sh` holds no settings of its own. They live in
  `CMakePresets.json` (S-20).
- **Next:** CI (W-84), and backfilling the seeded-bug catalogue for the
  older modules (W-51).

## What to do next, in order

1. Build and test everything on GCC 16 (W-48). Nothing after `journal/`
   has been confirmed there.
2. Profile the 70 ns around the book (W-24).
3. Measure on real hardware (W-50), and put the numbers in
   `benchmarks.md`.
4. Settle D-01 and D-14 together, then D-02.
5. Snapshots, once, for both feed and journal (D-05, W-10, W-72).
6. Decide D-12 and D-13 once the time to durable has been measured (W-80).
