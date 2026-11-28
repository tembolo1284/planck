# Quick start

From a fresh checkout to a venue with clients trading on it.

## 1. What you need

| Tool | Version | Notes |
|------|---------|-------|
| Linux, x86-64 | any recent kernel | WSL2 works for everything except latency numbers |
| GCC or Clang | GCC 13+, Clang 18+ | both if you want the whole build matrix |
| CMake | 3.25+ | |
| Ninja | any | |
| Python | 3.8+ | for the tools and three of the tests. Standard library only. |
| bash | 4.4+ | |

Optional: Python's `cffi` package turns on one more test of the C library.
GoogleTest is fetched during the first configure if it is not installed, so
that one needs network access.

On Debian or Ubuntu:

```bash
sudo apt install g++ clang cmake ninja-build python3 python3-cffi
```

## 2. Build and test

```bash
./build.sh test --checked
```

This configures `build/gcc-checked/`, builds everything and runs 370 tests.
It should end with `100% tests passed`. "checked" is an optimised build with
every contract tested at run time, which is the right one for tests.

If your GCC 13 or later is not the plain `g++` on your path:

```bash
./build.sh test --checked --cxx g++-16 --cc gcc-16
```

## 3. See it trade

```bash
scripts/demo.sh
```

This builds a release, starts a venue on loopback, attaches a market data
viewer, and has two clients trade with each other. It prints what each of
the four saw. Read the output top to bottom once: it is the whole system in
thirty lines.

## 4. Drive it by hand

Three terminals. The paths assume the default release build, which
`./build.sh` with no arguments makes.

```bash
# terminal 1: the venue, one shard, symbols 1 to 8
build/gcc-release/apps/planckd --feed 127.0.0.1:30001 --stats 5

# terminal 2: a client
build/gcc-release/apps/planck-client --client-id 1
sell 1 10 1000
```

```bash
# terminal 3: another client
build/gcc-release/apps/planck-client --client-id 2
buy 1 4 1000
```

Client 2 sees its order accepted and executed. Client 1 sees its resting
order partly filled. When a client leaves, whatever it still has resting is
cancelled. Type `help` at either prompt for the commands:

```
buy  SYMBOL QTY PRICE [day|ioc|fok]   limit order. PRICE is in ticks.
sell SYMBOL QTY PRICE [day|ioc|fok]
buy  SYMBOL QTY market
cancel ORDER_ID | cancel last
```

The client sends heartbeats by itself while you think. A client that goes
silent for five seconds is taken for dead and its orders are cancelled.

When a client leaves it prints where it got to, for example
`left at --epoch 1795800218734426517 --next-report 3`. Start it again with
those two options and the venue first sends whatever that client was sent
since: here the `Canceled` for the order it left behind. `--next-report 1`
alone asks for everything the venue still holds for the client.

To watch the public feed, in a fourth terminal:

```bash
build/gcc-release/apps/planck-mdtap --listen 127.0.0.1:30001
```

`--feed 127.0.0.1:30001` above sends the feed to a plain loopback address,
which works everywhere. Without it the venue uses the multicast group
239.255.42.1, which some hosts (WSL2 among them) do not route.

## 5. Survive a restart

```bash
build/gcc-release/apps/planckd --feed 127.0.0.1:30001 --journal /tmp/planck-journal
```

Trade as above, then kill the venue with `kill -9` and start it again with
the same command. It prints `shard 0 replayed N commands`: the books and the
whole market data stream are rebuilt from the journal. Then every order
that was resting is cancelled, because nobody is connected to a venue that
has just started. `planck-mdtap --from-start` shows all of it. See
[operations.md](operations.md).

## 6. Measure it

```bash
./build.sh bench
scripts/check_host.sh
```

The first times the pieces of the engine and prints percentiles. The second
tells you how far to trust them on this machine. See
[benchmarks.md](benchmarks.md).

## 7. Use the engine as a library

`build/gcc-release/capi/libplanck.so` and `capi/include/planck.h` are all a
C program needs. `capi/tests/capi_smoke.c` is a complete example, and
[c_api.md](c_api.md) explains it.

## Where next

- [building.md](building.md) for every build option.
- [testing.md](testing.md) for what the tests cover and how to run parts of them.
- [architecture.md](architecture.md) for how it works.

## If something goes wrong

| Symptom | Cause and fix |
|---------|---------------|
| `build/ holds an old build made without presets` | An older checkout built straight into `build/`. Run `rm -rf build`. |
| `ninja is not installed or not on PATH` | Install Ninja. The presets use it. |
| Configure hangs or fails fetching GoogleTest | No network. Install `libgtest-dev`, version 1.14 or later. |
| 369 tests, not 370 | Python's `cffi` is not installed. Harmless. |
| One fewer again, and no `rust_wire` | `cargo` is not installed. Harmless unless you are changing the wire protocol. |
| `FeedMulticast` reported as skipped | This host does not route multicast. Harmless. |
| `clang-asan` or `clang-tsan` fails to link | Clang's sanitizer runtime is missing. Install `libclang-rt-dev`. |
| A test fails only when the machine is busy | Tell us. Several such bugs were real (F-04 to F-06 in weaknesses.md). |
