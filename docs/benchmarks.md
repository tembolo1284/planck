# Benchmarks

Two programs measure planck. `planck-bench` times the pieces of the engine
inside one process. `planck-loadgen` drives a running `planckd` over TCP and
times it from outside, the way a client would see it.

Both keep every sample in a histogram and print percentiles, because for a
matching engine the figure that matters is the slow case. An average hides
it.

## Getting numbers worth trusting

1. Build Release. The benchmarks say so on their first line if you did not.

       ./build.sh --release

2. Ask the machine what it thinks of itself: `scripts/check_host.sh`. Every
   warning it prints is a source of tail latency.
3. Pin the threads, and give them cores nothing else uses. Best is cores
   taken out of the scheduler at boot with `isolcpus=` and `nohz_full=`.
4. Not under WSL2 or in a shared virtual machine, where another tenant can
   take the core at any moment. See W-30 in `weaknesses.md`.
5. Look at p99 and beyond, and run more than once. A `max` that moves by a
   factor of ten between runs is the machine, not the engine.

## planck-bench

    build/gcc-release/bench/planck-bench --cpu 2 --cpu2 3

| Option | Meaning |
|--------|---------|
| `--list` | Name the benchmarks and stop. |
| `--filter TEXT` | Run only benchmarks whose name contains TEXT. |
| `--samples N` | Operations timed per benchmark. Default 1,000,000. |
| `--cpu N` | Pin the measuring thread. |
| `--cpu2 N` | Pin the second thread of the two-thread benchmarks, and let it spin. Give this whenever you give `--cpu`, or those benchmarks crawl (W-87). |
| `--csv` | Comma separated output, for scripts. |
| `--quick` | A few thousand samples. Checks that everything runs. |

Every figure is nanoseconds for one operation.

| Row | What is timed |
|-----|---------------|
| `timer.empty_interval` | A start and a stop with nothing between. The smallest of these is taken off every other row, so no row includes the cost of timing it. |
| `book.add` | One order coming to rest in a book that holds 16,384 orders. |
| `book.cancel` | One cancel, of an order picked at random from that book. |
| `book.match_1_fill` | An incoming order that fills one resting order in full. |
| `book.match_10_fills` | An incoming order that fills ten. |
| `ring.push_pop` | One 32-byte command pushed and popped on one thread. |
| `ring.core_to_core` | One message stamped on one thread and read on another, with the ring empty each time. |
| `wire.decode_new_order` | What the gateway does to each request: find the frame, copy it out, check every field, build the command. |
| `shard.rest`, `shard.cancel`, `shard.trade` | A whole command through a shard, on the measuring thread: off the command ring, clock read, match, replies encoded and put on the outgoing rings. `trade` is one order filling one other. |
| `shard.*.journal` | The same with journaling on. The difference is all that journaling costs the matching thread. |
| `shard.*.disconnect` | The same with cancel on disconnect on, as a venue runs. The difference is what keeping each session's list of resting orders costs. |
| `shard_thread.*` | A shard on its own thread, as in the venue. From the moment a command is stamped to the moment its first report is read back, with one command in flight at a time. Two ring hops and the match. |

## planck-loadgen

Start a venue, then point the generator at it.

    build/gcc-release/apps/planckd --cpus 2-4 &
    build/gcc-release/bench/planck-loadgen --cpu 6 --sessions 4 --orders 1000000

It logs in `--sessions` connections and sends new orders. Most rest. A share
set by `--cross` (10 percent by default) cross and trade. Each session
cancels its oldest resting order once it holds more than `--depth`, so the
book stays the same size for the whole run, and at the end everything left
resting is cancelled. (The venue would cancel it anyway when the sessions
disconnect.) `--band-low`, `--band-ticks` and `--symbols` must
match what `planckd` was started with.

There are two ways to pace the load, and they answer different questions.

**Closed loop** (the default). Each session keeps `--window` orders
unanswered and sends the next when one is answered. This measures a round
trip with nothing queued. It cannot show overload, because the generator
slows down whenever the venue does.

**Fixed rate** (`--rate N`). N orders a second, on a schedule fixed before
the run starts. Two rows are printed:

| Row | Measured from | To |
|-----|---------------|----|
| `order.sent_to_answer` | the order was written to the socket | its answer was read |
| `order.due_to_answer` | the order was due on the schedule | its answer was read |

When the venue keeps up the two agree. When it falls behind, orders go out
late, and only the second row counts the time they spent waiting to be
sent. That row is what a client sending at that rate would have lived
through. Raise the rate until it climbs away from the first: that is the
venue's limit.

The summary line also reports rejects (and how many were Busy, the venue
shedding load), fills, cancels, and the most orders any session had
unanswered at once.

## Keeping and comparing runs

    scripts/bench.sh --cpus 2,3 --venue --venue-cpus 4-6 --loadgen-cpu 7

runs both programs and saves their output as CSV under `bench-results/`,
with a note of the machine, kernel and compiler. If an earlier run is there
it compares the new one with it. To compare any two by hand:

    tools/bench_compare.py bench-results/OLD bench-results/NEW

A row is flagged when its median grew by more than 10 percent or its p99 by
more than 25. On a shared or unpinned machine rows move that much on their
own, so a flag there is a reason to run again and no more.

## Results

### Development sandbox, 2026-11-26

A shared virtual machine with two CPUs at 2.1 GHz, GCC 13.3, Release,
`planck-bench --cpu 0 --cpu2 1`. **These are not the engine's numbers.**
They show that the benchmarks work and roughly where the time goes. The
tail columns in particular are the hypervisor taking the core away.

| benchmark (ns) | min | p50 | p90 | p99 | p99.9 |
|----------------|----:|----:|----:|----:|------:|
| timer.empty_interval | 20.0 | 26.7 | 29.5 | 43.3 | 115.7 |
| book.add | 11.4 | 16.2 | 21.9 | 34.8 | 75.7 |
| book.cancel | 9.5 | 23.8 | 29.5 | 104.3 | 197.6 |
| book.match_1_fill | 11.4 | 16.2 | 18.1 | 29.5 | 39.5 |
| book.match_10_fills | 50.5 | 140.5 | 171.0 | 209.0 | 304.3 |
| ring.push_pop | 2.9 | 5.7 | 6.7 | 11.4 | 17.1 |
| ring.core_to_core | 32.4 | 212.9 | 281.4 | 89,721 | 1,248,304 |
| wire.decode_new_order | 7.6 | 19.0 | 21.9 | 25.7 | 43.3 |
| shard.rest | 71.4 | 85.2 | 96.7 | 125.2 | 201.4 |
| shard.cancel | 66.7 | 87.1 | 117.6 | 193.8 | 334.8 |
| shard.trade | 88.6 | 121.4 | 129.0 | 155.7 | 216.7 |
| shard.rest.journal | 70.5 | 87.1 | 92.9 | 119.5 | 182.4 |
| shard.cancel.journal | 67.6 | 89.0 | 108.1 | 182.4 | 289.0 |
| shard.trade.journal | 87.6 | 125.2 | 129.0 | 151.9 | 224.3 |
| shard_thread.rest | 419.0 | 685.2 | 837.6 | 1,005 | 15,360 |
| shard_thread.cancel | 399.0 | 639.5 | 791.9 | 974.8 | 14,141 |
| shard_thread.trade | 414.3 | 746.2 | 883.3 | 1,036 | 15,360 |

What can be read from them even so:

- The book does its work in about 16 to 24 ns. A whole command through the
  shard takes about 85 ns. Most of the time on the matching path is spent
  around the book, not in it. See W-24.
- Journaling adds nothing visible to the matching thread. See D-12.
- Nor does cancel on disconnect, as far as this machine can tell: three
  runs of `shard.cancel` gave medians of 121, 129 and 133 ns, and three of
  `shard.cancel.disconnect` gave 85, 91 and 167. The rows are not in the
  table for that reason. See W-88.
- Crossing to another thread and back costs several times what the match
  does. On two shared CPUs that figure is mostly the machine.

`planck-loadgen` against `planckd --idle yield` on the same two CPUs, four
sessions at a fixed 20,000 orders a second: 9.8 microseconds at the median
from send to answer and 27 at p99. From the due time, 10.2 and 64. Five
busy threads on two CPUs, so again a check that the tool works and no more.

### Real hardware

Not measured yet. See W-50.
