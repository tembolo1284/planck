# Operations

Running `planckd` and the tools around it. This is honest about how little
operational machinery exists yet: no config file for symbols, no log file,
no monitoring endpoint (W-60, W-64).

## planckd

```
planckd [options]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--listen ADDRESS:PORT` | `0.0.0.0:9000` | Order entry, TCP |
| `--feed ADDRESS:PORT` | `239.255.42.1:30001` | Market data, UDP. Shard N sends to PORT + N. A multicast group or a plain address. |
| `--retransmit ADDRESS:PORT` | `0.0.0.0:30000` | Where feed retransmit requests arrive |
| `--shards N` | 1 | Matching threads, 1 to 16 |
| `--symbols N` | 8 | Symbols traded, ids 1 to N. Symbol S trades on shard S mod the shard count. |
| `--band-low TICKS` | 0 | Lowest price accepted, every symbol |
| `--band-ticks N` | 65536 | Prices per symbol. A power of two, 64 to 262144. |
| `--orders N` | 1048576 | Resting orders per shard |
| `--clients FILE` | off | Who may log in. Without it anyone may, as any client id. See "Clients" below. |
| `--journal DIRECTORY` | off | Record every command, and rebuild from it at startup |
| `--journal-sync MS` | 10 | Longest a command may wait to reach the disk. 0 syncs after every write. |
| `--cpus LIST` | none | Pin threads, for example `2-5` or `2,4,6,8` |
| `--idle spin\|yield\|sleep` | spin with `--cpus`, else sleep | What an idle thread does |
| `--huge off\|try\|require` | try | 2 MiB pages |
| `--lock-memory` | off | Keep every page in RAM |
| `--stats SECONDS` | 0 | Print counters this often |

Stop it with Ctrl-C or SIGTERM.

### What it prints

At startup, one line per fact, then `ready`:

```
planckd 2 shards, 8 symbols (ids 1 to 8), prices 0 to 65535
feed stream 0 -> 127.0.0.1:30001
feed stream 1 -> 127.0.0.1:30002
order entry on 0.0.0.0:9000
clients: 12 listed in /etc/planck/clients
retransmit requests on 0.0.0.0:30000
journal in /var/planck, synced within 10 ms
shard 0 replayed 18234 commands
shard 1 replayed 17790 commands
ready
```

Scripts should wait for `ready` before connecting. On the way down it prints
`stopping` and then the counters one last time.

### Clients

`--clients FILE` names a text file with one client per line:

```
# desk A
1 1001
2 0x7d2          # tokens may be hexadecimal
17 18364758544493064720
```

The first number is the client id, from 1 to 65,535, listed once. The
second is that client's token, any 64-bit number. Anything after a `#` is a
comment. A file with a mistake in it stops the venue from starting, and the
message names the line. At most 4,096 clients.

- A client logs in with its id and token: `planck-client --client-id 17
  --token 18364758544493064720`. The token is decimal on the command line.
- The client id is the session id. It owns the client's orders, on every
  connection and across restarts, so do not hand a retired client's id to
  someone new while its orders may still be in the book or the journal.
- The file is read once. Changing it means a restart (W-09).
- Keep it readable by the venue's user only: `chmod 600`. Tokens are stored
  as written.
- Make tokens random. `od -An -N8 -tu8 /dev/urandom` prints one.

**Without `--clients`** the venue prints `clients: anyone may log in` and
means it. Any client id from 1 to 65,535 is accepted and the token is
ignored. Anyone who can reach the port can log in as a client that is not
connected, cancel its orders and receive its fills (W-05). That is right
for tests, benchmarks and the demo, and for nothing else.

**The network.** The token is sent in the clear and is the same every time
(W-08). It stops one client posing as another. It does not stop anyone who
can read or alter the traffic. So order entry must run over a link that
strangers are not on: a private network, or a tunnel. For clients on home
or office internet, put the venue and each client on a WireGuard network
and have `planckd --listen` bind the venue's tunnel address, not `0.0.0.0`.
Nothing in planck sets this up, and it has not been tried here (W-86).

### Threads and CPUs

A venue with N shards runs N + 2 hot threads, or N + 3 with a journal.
`--cpus` hands out CPUs in this order: shards, gateway, publisher, recorder.
So `--shards 2 --cpus 2-6 --journal DIR` puts the shards on 2 and 3, the
gateway on 4, the publisher on 5 and the recorder on 6.

With `--cpus`, every thread spins and holds its core at 100 percent. That is
intended. Without it, threads sleep 50 microseconds when idle and a round
trip costs a few hundred microseconds (D-11). An unpinned venue is for
development.

### Preparing a host for low latency

`scripts/check_host.sh` checks the list below and says what is missing.
None of this has been exercised in development (W-43, W-49).

| Setting | How | Why |
|---------|-----|-----|
| Isolated cores | Kernel command line: `isolcpus=2-6 nohz_full=2-6 rcu_nocbs=2-6` | Nothing else is scheduled on the venue's cores |
| Performance governor | `cpupower frequency-set -g performance` | No frequency changes mid-order |
| Huge pages | `echo 1024 > /proc/sys/vm/nr_hugepages`, then `--huge require` | Fewer TLB misses on the price ladder and order store |
| Locked memory | `ulimit -l unlimited`, then `--lock-memory` | Nothing is ever paged out |
| No SMT sibling | Leave the sibling of each venue core idle, or turn SMT off | A sibling shares the core's cache and execution units |
| Stable counter | `constant_tsc` and `nonstop_tsc` in `/proc/cpuinfo` | The clock depends on it |
| Not a virtual machine | | See W-30 for what WSL2 does to clocks |

### Counters

`--stats N` prints five lines every N seconds.

```
stats engine  commands accepted rejected trades cancels resting stalls
stats gateway sessions connections logins messages rejected busy reports
              dropped protocol_errors slow_consumers bad_credentials
stats feed    messages datagrams heartbeats send_failures retransmit_requests
              retransmitted refused
stats latency reports p50_ns p99_ns p999_ns max_ns
stats journal records bytes syncs discarded failed
```

What to watch:

| Counter | If it is not zero, or is climbing |
|---------|-----------------------------------|
| `engine stalls` | A consumer of the shard's output is too slow. The shard waited. |
| `gateway busy` | Orders were rejected because a command ring was full. The shard is the bottleneck. |
| `gateway dropped` | Reports for clients that had no connection at the time. Expected after a disconnect (W-01). |
| `gateway bad_credentials` | Logins refused: not listed, or the wrong token. A steady climb is someone guessing. |
| `gateway slow_consumers` | Clients disconnected for not reading their reports (D-03) |
| `gateway protocol_errors` | Clients sent bytes that are not the protocol |
| `feed send_failures` | The kernel refused datagrams. Subscribers will have gaps. |
| `feed refused` | Retransmit requests for data older than the history. Those subscribers are lost (W-10). |
| `journal failed` | **The journal has stopped. See below.** |
| `journal discarded` | A torn tail was cut off at startup. Normal after a crash. |

`stats latency` is request-off-the-socket to report-ready-to-send, inside
the process. It leaves out the kernel on both sides (W-66). Use
`planck-loadgen` for what a client sees.

Counters reach the printout up to 1024 loop passes late, so the last digits
of two lines printed together may not agree.

## Journal and restart

With `--journal DIR` the venue keeps one file per shard, `shard-NN.journal`.

**At startup** each shard replays its file into an empty book before the
venue accepts connections. Replay also republishes the whole market data
stream from sequence 1 (W-73). Subscribers that were already listening
ignore what they have seen. Restart time grows with the journal, since
there are no snapshots (W-72).

**What survives what:**

| Event | Lost |
|-------|------|
| Clean stop | Nothing |
| Process killed or crashed | Nothing. The kernel has the data. |
| Power cut or kernel crash | Up to `--journal-sync` milliseconds of commands that clients were already told about (W-70) |

**The venue will refuse to start** when:

| It reports | Cause | What to do |
|--------------|-------|------------|
| `InvalidArgument` while rebuilding a shard | `--shards`, `--symbols`, `--band-*` or `--orders` differ from the run that wrote it (W-74) | Use the old flags, or move the journal away and start empty |
| `Unsupported` while rebuilding a shard | The journal is in an older format. Journals written before sessions became client ids are format 1, and this venue reads format 2 (S-26). | Move the journal away and start empty |
| `Protocol` while rebuilding a shard | A bad record with good ones after it (S-13) | Inspect with `planck-replay`. Decide by hand. There is no repair tool. |

**If the journal fails while running** (disk full or failing), `planckd`
prints `THE JOURNAL HAS FAILED` to stderr and keeps matching (W-71, D-13).
From that moment a restart loses everything after the failure. Stop the
venue.

**Starting a new day:** stop the venue, move the journal directory away,
start again. There is no rotation.

**Sessions:** after a restart, clients must log in again. Each is the same
session as before, because a session id is a client id. Orders from before
the restart still rest in the book and still belong to their clients, who
can cancel them. Fills that happened while a client was away are not
reported to it (W-01). Once cancel on disconnect is built, a restart will
cancel every resting order (D-14).

## The other programs

### planck-client

An interactive order entry client. Reads commands from the terminal or a
pipe.

```
planck-client [--connect ADDRESS:PORT] [--client-id N] [--token N] [--linger MS]
               defaults: 127.0.0.1:9000, 1, 0, 200
```

```
buy  SYMBOL QTY PRICE [day|ioc|fok]
sell SYMBOL QTY PRICE [day|ioc|fok]
buy  SYMBOL QTY market
sell SYMBOL QTY market
cancel ORDER_ID
cancel last
ping
wait MS
help
quit
```

After its input ends it keeps listening until the venue has been quiet for
`--linger` milliseconds, to catch late fills. It sends one request and waits for the answer, so it cannot
generate load (W-63).

### planck-mdtap

Prints the market data feed.

```
planck-mdtap [--listen ADDRESS:PORT] [--stream N] [--retransmit ADDRESS:PORT]
             [--from-start] [--count N] [--quiet]
```

`--listen` defaults to 239.255.42.1:30001 plus the stream number. With
`--retransmit` it recovers lost messages, and without it a gap is only
reported. `--from-start` asks the venue to resend from sequence 1, as far as
its history reaches, and needs `--retransmit`. `--count N` exits after N
messages. `--quiet` prints one summary line a second in place of every
message. It prints events and does not build books (W-62).

### planck-replay

Checks and prints a journal file, offline.

```
planck-replay --file PATH [--dump]
```

| Exit | Meaning |
|-----:|---------|
| 0 | Intact |
| 3 | Torn tail. Normal after a crash. The venue will cut it off. |
| 4 | Damaged. The venue will refuse it. |
| 1 | Unreadable, or not a journal |

### planck-loadgen and planck-bench

See [benchmarks.md](benchmarks.md).

## Scripts

| Script | Does |
|--------|------|
| `scripts/demo.sh` | Starts a venue, a feed viewer and two clients on loopback, and shows what each saw |
| `scripts/check_host.sh` | Says how far to trust latency numbers from this machine |
| `scripts/bench.sh` | Runs the benchmarks, saves the results under `bench-results/`, compares with the previous run |
| `scripts/check_all.sh` | Builds and tests the build matrix |

## Known operational gaps

Before putting this in front of anyone, read the Gateway, Feed, Journal and
Programs sections of [weaknesses.md](weaknesses.md). In short: a login
token sent in the clear, no cancel on disconnect, no heartbeats or idle
timeout, no feed snapshots, an unprotected retransmit port, asynchronous
durability, no clean shutdown for clients, and no monitoring beyond stdout.
