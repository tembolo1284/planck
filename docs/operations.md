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
| `--heartbeat MS` | 1000 | Send a heartbeat to a client that has been sent nothing for this long. 0 never does. |
| `--idle-timeout MS` | 5000 | End a connection nothing has been heard on for this long, and cancel its orders. Longer than `--heartbeat`. 0 never does, and is the only value allowed with `--heartbeat 0`. |
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
clients: one heard nothing from for 5000 ms is disconnected
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
- The client id is the session id. It owns the client's orders, and it is
  how the journal names the client, so do not hand a retired client's id to
  someone new if the journal should stay readable as a record.
- The file is read once. Changing it means a restart (W-09).
- Keep it readable by the venue's user only: `chmod 600`. Tokens are stored
  as written.
- Make tokens random. `od -An -N8 -tu8 /dev/urandom` prints one.

**Without `--clients`** the venue prints `clients: anyone may log in` and
means it. Any client id from 1 to 65,535 is accepted and the token is
ignored. Anyone who can reach the port can log in as a client that is not
connected and trade as that client (W-05). That is right for tests,
benchmarks and the demo, and for nothing else.

**The network.** The token is sent in the clear and is the same every time
(W-08). It stops one client posing as another. It does not stop anyone who
can read or alter the traffic. So order entry must run over a link that
strangers are not on: a private network, or a tunnel. For clients on home
or office internet, put the venue and each client on a WireGuard network
and have `planckd --listen` bind the venue's tunnel address, not `0.0.0.0`.
Nothing in planck sets this up, and it has not been tried here (W-86).

### Disconnects and restarts

A venue never leaves a client's orders working behind its back (S-27).

- When a connection that had logged in ends, however it ends, everything
  that client has resting is cancelled, in every shard. The feed shows each
  order leaving.
- When the venue starts, everything is cancelled before the first client
  can connect (S-28). With a journal the books are rebuilt first and then
  emptied, so the feed shows that too.
- A client that reconnects is the same session and starts with nothing
  resting. It is not told what happened while it was away (W-04).

There is no flag for this. A connection ends in one of three ways:

- The client closes it or logs out, or the kernel reports it broken.
- The venue drops it: for bytes that are not the protocol, or for not
  reading its reports.
- Nothing is heard on it for `--idle-timeout`. This is how a line that died
  without closing is noticed. Every client is told the timeout when it logs
  in and is expected to send heartbeats well inside it. `planck-client`
  does so by itself. Watch `idle_timeouts` in the counters.

Choosing `--idle-timeout`: it is how long a dead client's orders can go on
trading, so shorter is safer. But a client on a poor line can go silent for
seconds while TCP resends, and dropping it cancels its orders. Five seconds
suits home and office internet. On a private network one or two seconds is
usual. The gateway judges silence only while it is running itself: if it
was held up for 100 ms or more, every connection gets a fresh start.

One limit to know:

- All of a client's orders are cancelled in one go, and the shard does
  nothing else meanwhile. A client with hundreds of thousands of orders
  resting makes everyone on its shards wait when it leaves (W-25).

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
stats engine  commands accepted rejected trades cancels disconnect_cancels
              resting stalls
stats gateway sessions connections logins messages rejected busy reports
              dropped protocol_errors slow_consumers bad_credentials
              session_closes heartbeats idle_timeouts login_timeouts
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
| `engine disconnect_cancels` | Orders cancelled because their client's connection ended, or the venue started. Not included in `cancels`. |
| `engine commands` | Counts sessions closing as well as orders and cancels. One per shard for each client that leaves, and one per shard at each start. |
| `gateway dropped` | Reports for clients that had no connection at the time. Each order cancelled by a disconnect adds one. |
| `gateway idle_timeouts` | Connections ended because nothing was heard on them. Each is a client whose line or program died, or one that does not send heartbeats. A burst of them at once points at the network, or at a timeout set too short. |
| `gateway login_timeouts` | Connections that kept talking but never logged in. |
| `gateway session_closes` | Times a shard was told that a client had gone: the number of shards for each logged-in connection that ended. |
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
ignore what they have seen. Then every order that is resting is cancelled,
which the feed shows and the journal records. Restart time grows with the
journal, since there are no snapshots (W-72).

The journal is what keeps the feed whole across a restart, and the record
of what the venue did. It does not carry resting orders over: nothing does.

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
| `Unsupported` while rebuilding a shard | The journal is in an older format. This venue reads format 3 only (S-26, S-30). | Move the journal away and start empty |
| `Protocol` while rebuilding a shard | A bad record with good ones after it (S-13) | Inspect with `planck-replay`. Decide by hand. There is no repair tool. |

**If the journal fails while running** (disk full or failing), `planckd`
prints `THE JOURNAL HAS FAILED` to stderr and keeps matching (W-71, D-13).
From that moment a restart loses everything after the failure. Stop the
venue.

**Starting a new day:** stop the venue, move the journal directory away,
start again. There is no rotation.

**Sessions:** after a restart, clients must log in again. Each is the same
session as before, because a session id is a client id, and each finds
that nothing of its rests any more. What filled before the venue went down
is not reported again (W-04).

## The other programs

### planck-client

An interactive order entry client. Reads commands from the terminal or a
pipe.

```
planck-client [--connect ADDRESS:PORT] [--client-id N] [--token N] [--linger MS]
               defaults: 127.0.0.1:9000, 1, 0, 200
```

It sends heartbeats by itself, as often as the venue's timeout calls for,
and gives up if it hears nothing from the venue for twice that timeout.
`ping` sends one at once. The venue does not answer it.

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
token sent in the clear, no report recovery, no feed snapshots, an unprotected retransmit port, asynchronous
durability, no clean shutdown for clients, and no monitoring beyond stdout.
