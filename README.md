# planck

A venue-side limit order matching engine in C++23, built for nanosecond tail
latency.

planck is the part of an exchange that sits behind the network: it accepts
orders over TCP, matches them by price and time, tells each client what
happened to its orders, and publishes the public record of the market over
UDP. It is a from-scratch, speed-first design. One thread owns each set of
books outright, threads talk only through lock-free rings, and nothing on the
matching path allocates, locks, or makes a system call.

## Status

Every directory has had a first pass and is tested. Nothing has been measured
on real hardware yet, and several product decisions are still open. Read
[docs/weaknesses.md](docs/weaknesses.md) before relying on any of it.

| | |
|---|---|
| Language | C++23, no exceptions, no RTTI, warnings as errors |
| Compilers | GCC 13 or later, Clang 18 or later |
| Platform | Linux on x86-64. AArch64 paths exist and are untested. |
| Build | CMake 3.25 or later with Ninja, driven by `./build.sh` |
| Tests | 370, in ten build configurations |
| Size | about 7,700 lines of library, 7,900 of tests |

## Quick start

```bash
./build.sh test --checked     # build and run every test
scripts/demo.sh               # a venue, a feed viewer and two clients trading
./build.sh bench              # time the pieces of the engine
./build.sh help               # everything else
```

More in [docs/quick_start.md](docs/quick_start.md).

## What is in the box

```
include/planck/   the library's headers, one directory per module
src/              the library's sources, same layout
apps/             planckd (the venue), planck-client, planck-mdtap, planck-replay
bench/            planck-bench (in process) and planck-loadgen (over TCP)
capi/             libplanck.so: the matching engine behind a C interface
rust/             planck-wire: the wire protocol as a Rust crate, for clients
tests/            unit, property, integration and end-to-end tests
tools/            seeded-bug runner, benchmark comparison, style check
scripts/          build matrix, benchmark runs, demo, host check
docs/             everything below
build.sh          configure, build, test, clean
CMakePresets.json the ten build configurations
```

The modules, from the bottom up:

| Module | What it is |
|--------|------------|
| `core` | Strong types, status codes, contracts, compile-time limits |
| `sys` | Cycle-counter clock, CPU pinning, huge pages, memory locking |
| `mem` | Arena, index-addressed pool, cache-line alignment |
| `book` | The order book: flat price ladder, bitmaps, price-time matching |
| `ring` | Single-producer single-consumer wait-free queue |
| `wire` | The binary protocol: message layouts, framing, validation |
| `engine` | The shard: one matching thread and everything it owns |
| `net` | Non-blocking TCP and UDP sockets, an epoll wrapper |
| `gateway` | Order entry: client sessions in, reports out |
| `feed` | Market data: sequenced UDP out, gap recovery in |
| `perf` | Latency histogram |
| `journal` | Command log on disk, and replay from it at restart |

## How it fits together

```
 clients                                                      subscribers
    |  TCP                                                         ^  UDP
    v                                                              |
+---------+  commands   +---------+  market events   +-----------+
| gateway | ----------> |  shard  | ---------------> | publisher |
|         | <---------- |         |                  +-----------+
+---------+   reports   |  books  |  journal entries +-----------+
                        +---------+ ---------------> | recorder  | --> disk
                                                     +-----------+
```

Each box is one thread. Each arrow is a ring with exactly one writer and one
reader. A venue runs one shard per group of symbols, and every shard has its
own four rings.

## Documentation

| Read this | When you want to |
|-----------|------------------|
| [quick_start.md](docs/quick_start.md) | Get it built, see it trade, in ten minutes |
| [building.md](docs/building.md) | Know every build option, configuration and target |
| [testing.md](docs/testing.md) | Run tests, understand what they prove, add your own |
| [architecture.md](docs/architecture.md) | Understand the design, module by module |
| [protocol.md](docs/protocol.md) | Write a client, a feed handler or a journal reader, in C++, Rust or anything else |
| [operations.md](docs/operations.md) | Run the venue: flags, pinning, restart, counters |
| [c_api.md](docs/c_api.md) | Use the engine from C, Python or anything with an FFI |
| [benchmarks.md](docs/benchmarks.md) | Measure it, and read the numbers correctly |
| [contributing.md](docs/contributing.md) | Change the code: rules, workflow, taking over a module |
| [decisions.md](docs/decisions.md) | See what was decided, why, and what is still open |
| [weaknesses.md](docs/weaknesses.md) | See what is wrong, missing or unproven |

If you are taking over part of the project, start with
[architecture.md](docs/architecture.md), then the "Taking over a module"
section of [contributing.md](docs/contributing.md). What is being built
next, and in what order, is the "Agreed plan" at the top of
[decisions.md](docs/decisions.md).

## Where it stands

What the venue does today:

- **Matches** limit, market, IOC and FOK orders in price and time priority,
  on one thread per shard, with nothing allocated after startup.
- **Knows its clients.** `planckd --clients` says who may log in. A client
  is the same session on every connection (S-23, S-24).
- **Cancels what a client leaves behind.** A client's orders are cancelled
  when its connection ends, and every order when the venue starts (S-27,
  S-28). A line that dies without closing is noticed by its silence, after
  five seconds by default (S-31).
- **Lets a client catch up.** Reports are numbered, and a client that
  reconnects is sent the ones it missed. An order sent twice is carried out
  once (S-33, S-34).
- **Rebuilds itself.** With `--journal`, a restart replays every command
  and arrives at the same books and the same market data stream.
- **Publishes market data** over UDP with gap recovery by retransmission.

What it does not do yet, most serious first. The full list, with ten to
read first, is [weaknesses.md](docs/weaknesses.md):

- **No trustworthy latency numbers exist.** The benchmarks work. Every
  figure so far is from a shared virtual machine or WSL2 (W-50).
- **Durability is asynchronous.** A command is answered before it is on
  disk. A process crash loses nothing. A power cut can lose up to 10 ms
  (W-70, D-12). Step 4 of the plan.
- **The feed has no snapshots.** A subscriber that falls too far behind
  cannot recover (W-10, D-05). Step 5.
- **Reports held for a client do not survive a restart,** and only the
  last 1024 for each client are held at all (W-98, W-99).
- **The login token is sent in the clear.** Order entry must run over a
  private link (S-23, W-08).
