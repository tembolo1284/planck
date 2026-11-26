# Decisions

Choices that shape planck and could reasonably have gone another way. The
open ones are to be settled once every directory has had a first pass. Until
then the code does what "Today" says.

Things that are simply broken or missing live in `weaknesses.md`.

## Open

### D-01 What happens to a client's orders when it disconnects
- **Today:** they stay in the book until they trade. Session ids are never reused, so nobody else can cancel them or receive their fills.
- **Alternative A:** cancel on disconnect. This is what most venues do. It needs a per-session list of live orders, kept by the shard.
- **Alternative B:** keep the orders and let the same client id reclaim them after logging in again. This needs authentication (D-02) and report replay (W-04).
- **Leaning:** A, as a per-session flag chosen at login.
- **Touches:** W-01, W-02.

### D-02 Authentication
- **Today:** none. The login token is accepted and ignored.
- **Alternatives:** a static table of client id and token loaded at start, or leave it to the network (private links, an authenticating proxy in front).
- **Leaning:** static table. It is small, and it makes D-01 B possible.
- **Touches:** W-05.

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

### D-09 Order of the remaining work
- **Original plan:** `journal/`, `perf/`, `capi/`, `apps/`, `bench/`, then `tools/`, `scripts/`, `docs/`, presets and README.
- **Proposed:** `apps/` first, so there is a venue to start and poke at, then the rest in the original order.

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
