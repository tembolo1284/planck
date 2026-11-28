# Protocol

Everything planck puts on a wire or a disk, byte by byte. Enough to write an
order entry client, a feed handler or a journal reader without reading the
source. The source of truth is `include/planck/wire/` and
`include/planck/journal/format.hpp`, where `static_assert`s pin every size
and offset given here.

There are two implementations, and both are checked against the same
bytes: the C++ one the venue runs, and a Rust one in `rust/planck-wire/`
for programs that talk to the venue without linking any C++. See
"Other languages" below.

## Common rules

- Protocol version 3. A client that logs in with any other is refused.
- Every integer is little-endian. There is no padding inside a message.
- Reserved bytes must be zero when sent. The venue rejects a client message
  whose reserved bytes are not zero.
- Prices are signed 32-bit ticks. Quantities are unsigned 32-bit.
- Timestamps are nanoseconds since the Unix epoch, unsigned 64-bit.
- Every message starts with the same 4-byte header:

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 2 | `length` | Size of the whole message in bytes, header included |
| 2 | 1 | `type` | Message type, from the tables below |
| 3 | 1 | `reserved` | Zero |

The largest message is 48 bytes. Offsets in the tables below count from the
start of the message, so every body starts at offset 4.

## Order entry (TCP)

One TCP connection per client. Messages follow each other with nothing
between them. A length or type that is not in this document closes the
connection.

### Sessions and connections

A **session** is a client's standing with the venue. Its id is the client id
the client logs in with, from 1 to 65,535. It owns the client's orders and
it is the same on every connection and across a restart of the venue. A
**connection** is one TCP link. At most one connection speaks for a session
at a time.

1. Client connects and sends `LoginRequest` with its client id and token,
   and the number of the first report it has not seen, if it has been
   connected before.
2. Venue answers `LoginAccepted`, whose session id is the client id. Or it
   answers `LoginRejected` and closes the connection: another try needs
   another connection.
3. Venue sends the reports the client missed, if it asked and they are
   still held.
4. Client sends `NewOrder` and `CancelOrder`. Venue sends reports.
5. Client sends `Logout` or just closes.

An order or cancel before step 2 is answered with `OrderRejected` or
`CancelRejected`, reason NotLoggedIn.

### Heartbeats and silence

A TCP connection can die without either end being told. So each side
speaks up when it has had nothing else to say, and each gives up on a line
it has heard nothing on for too long.

- **Neither side answers a `Heartbeat`.** It is not a ping. Two ends that
  both answered would never stop.
- **The venue** sends a `Heartbeat` to a logged-in client it has sent
  nothing else for its heartbeat interval, one second by default.
- **The venue ends a connection** it has heard nothing on for its idle
  timeout, five seconds by default. Anything the client sends counts. The
  timeout is in `LoginAccepted`, in milliseconds. Zero means the venue
  never does this.
- **A client** should send something, a `Heartbeat` if nothing else, about
  three times in each timeout. A client that hears nothing at all from the
  venue for two timeouts should take the venue for dead.
- **A connection has one timeout to log in**, whatever else it sends.

Being dropped for silence is a disconnect like any other: the session's
resting orders are cancelled.

**When a connection ends**, every order its session has resting is
cancelled. It does not matter how it ends: a `Logout`, a close, a broken
line the kernel has noticed, or the venue dropping the client for a
protocol error or for not reading its reports. Each cancelled order is
taken off the market data feed. A `Canceled` with reason 2 is produced for
its owner too, and held for it like any report it was not there to read.
Orders the client had sent just before the end are carried out first and
then cancelled like the rest.

So a client that connects again is the same session, with nothing resting.
What filled before the line dropped, and what was cancelled after, it
learns by asking for the reports it missed: see the next section. A client
that logs in again at once may be refused with "already logged in" for a
moment, while the venue finishes closing its last connection: try again.

A venue that restarts cancels every resting order before it accepts a
connection (S-28).

A line that dies without closing is noticed by the silence on it, as
described above, and treated the same way.

The token is sent in the clear. Order entry belongs on a private link
(S-23, W-08).

### Report numbers and recovery

The venue numbers the **reports** it sends a session: `OrderAccepted`,
`OrderRejected`, `Executed`, `Canceled` and `CancelRejected`, from the
moment the session has logged in. The first is 1 and each is one more than
the last, across however many connections the session uses. Nothing else
is numbered: not `LoginAccepted`, not `Heartbeat`, not a reject sent to a
connection that has not logged in.

**The numbers are not on the wire.** TCP loses nothing and reorders nothing
within a connection, so `LoginAccepted` says what number the next report
has (`next_report`) and the client counts from there, adding one for each
report it reads. `wire::is_report()` says which types to count. A client
that keeps that one number knows exactly what it has seen.

**To recover**, a client that connects again puts the number it expects
next in `LoginRequest.next_report`, with the `epoch` its last
`LoginAccepted` gave it. The venue then sends every report from that
number on, in order, before anything new. `LoginAccepted.live_from` is the
number the first new report will have, so the client knows when it has
caught up: the reports from `next_report` up to `live_from` are the old
ones. New reports produced meanwhile wait their turn behind them.

| `LoginRequest` says | The venue sends |
|---------------------|-----------------|
| `next_report` 0 | Nothing from before. `next_report` and `live_from` in the answer are equal. |
| `next_report` N, `epoch` the venue's own | Reports N onwards, or from the oldest still held if N is older. |
| `next_report` N, any other `epoch` | Every report still held, as if N were 1. |
| `next_report` above what exists, `epoch` the venue's own | `LoginRejected`, reason 4. The client has miscounted, or is not who it says. |

**What is held.** The venue holds the last `--retain-reports` reports of
each session, 1024 unless told otherwise, in memory (S-33). A client that
asks for older ones is not refused: `LoginAccepted.next_report` comes back
higher than what it asked for, and the reports in between are gone. That is
the only signal, so compare the two. A client that is connected and falls
further behind than what is held is disconnected as too slow.

**The epoch** names one run of the venue. Report numbers start again at 1
when the venue restarts, and nothing held survives (W-98). A client whose
`epoch` is not the venue's is told so by the `epoch` in the answer, and is
sent everything the new run holds for it: at least the `Canceled` for each
order the restart took off the book (S-28). It should take the restart as
having cancelled everything it had resting, and reconcile fills from
before it by order id and match id, or from the journal.

### Sending an order again

A client that loses its connection after sending an order cannot know
whether the order arrived. It does not need to. **`client_order_id` must be
above every id the session has sent before**, on any connection, and a
`NewOrder` whose id is not is taken for a repeat and ignored: no answer, no
second order (S-34). So the rule for a client is:

1. Number orders 1, 2, 3 and so on, and never reuse a number.
   `LoginAccepted.last_client_order_id` is the highest the venue has had, so
   a client that has lost its own count carries on from there.
2. After connecting again, recover the reports. They show what became of
   every order that arrived.
3. Send again, with the same ids and in the same order, any order that has
   no report. The ones that did arrive are ignored. The others are new.

An id is used as soon as the venue has read it from a logged-in connection,
whatever became of the order. One that was rejected, even with Busy, is
used: try again with the next id. Zero is never above anything, so ids
start at 1. The venue remembers the highest id across a restart, from its
journal, for every order that reached a matching shard.

A repeat gets no answer because the answer already exists: it is the
report of the original, which the client can ask for. An order sent with
an id below one it skipped earlier is also ignored, and that one has no
report anywhere, so send in order.

### Client to venue

**LoginRequest**, type 0x01, 40 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `protocol_version` | 3 |
| 6 | 2 | reserved | |
| 8 | 4 | `client_id` | Who you are, 1 to 65,535. One connection per client id at a time. |
| 12 | 4 | reserved | |
| 16 | 8 | `token` | The secret the venue holds for this client id. Ignored by a venue started without `--clients`. |
| 24 | 8 | `epoch` | From the `LoginAccepted` of an earlier connection. 0 if there was none. |
| 32 | 8 | `next_report` | The number of the first report you have not seen. 0: send nothing from before. 1: send everything held. |

**Heartbeat**, type 0x04, 4 bytes. Header only. Either side may send it.
Nobody answers it.

**Logout**, type 0x05, 4 bytes. Header only.

**NewOrder**, type 0x10, 32 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `symbol_id` | |
| 6 | 1 | `side` | 0 buy, 1 sell |
| 7 | 1 | `order_type` | 0 limit, 1 market |
| 8 | 8 | `client_order_id` | Yours, and above every one this session has sent before. Echoed in the answer. An order whose id is not above the last is ignored. |
| 16 | 4 | `price` | Ticks. Ignored for a market order. |
| 20 | 4 | `qty` | Greater than zero |
| 24 | 1 | `tif` | 0 day, 1 immediate or cancel, 2 fill or kill |
| 25 | 7 | reserved | |

**CancelOrder**, type 0x11, 16 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 4 | reserved | |
| 8 | 8 | `order_id` | The venue's id, from `OrderAccepted` |

### Venue to client

**LoginAccepted**, type 0x02, 40 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `session_id` | The client id you logged in with |
| 6 | 2 | `idle_timeout_ms` | The venue ends a connection it hears nothing on for this long. 0: it never does. |
| 8 | 8 | `epoch` | This run of the venue. Never 0. Keep it with your report count. |
| 16 | 8 | `next_report` | The number of the first report this connection will carry. Count from here. |
| 24 | 8 | `live_from` | The number the first new report will have. Those below it are from before this login. |
| 32 | 8 | `last_client_order_id` | The highest order id the venue has had from this session |

**LoginRejected**, type 0x03, 8 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `reason` | 0 bad credentials, 1 already logged in, 2 unsupported version, 3 server full (defined, not sent today), 4 bad sequence: `next_report` is above any report that exists |
| 5 | 3 | reserved | |

Bad credentials covers a client id of zero or above 65,535, a client that
is not listed, and a wrong token. The three are not told apart, on purpose.
A venue with no room for another connection closes it without a message.

**OrderAccepted**, type 0x20, 32 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 4 | reserved | |
| 8 | 8 | `client_order_id` | From your `NewOrder` |
| 16 | 8 | `order_id` | The venue's id. Use it to cancel. |
| 24 | 8 | `timestamp` | |

**OrderRejected**, type 0x21, 16 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `reason` | Reject code, below |
| 5 | 3 | reserved | |
| 8 | 8 | `client_order_id` | |

**Executed**, type 0x22, 48 bytes. One per fill, to each side.

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `liquidity` | 0 maker (your order was resting), 1 taker |
| 5 | 3 | reserved | |
| 8 | 8 | `order_id` | |
| 16 | 8 | `match_id` | Same on both sides of the trade, and on the feed |
| 24 | 4 | `price` | Always the resting order's price |
| 28 | 4 | `qty` | This fill |
| 32 | 4 | `leaves` | What is left of the order after it |
| 36 | 4 | reserved | |
| 40 | 8 | `timestamp` | |

**Canceled**, type 0x23, 32 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `reason` | 0 you asked, 1 unfilled rest of an IOC or market order, 2 the session lost its connection |
| 5 | 3 | reserved | |
| 8 | 8 | `order_id` | |
| 16 | 4 | `qty` | Quantity that was cancelled |
| 20 | 4 | reserved | |
| 24 | 8 | `timestamp` | |

**CancelRejected**, type 0x24, 16 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `reason` | Reject code, below |
| 5 | 3 | reserved | |
| 8 | 8 | `order_id` | |

### Reject codes

| Code | Name | Sent when |
|-----:|------|-----------|
| 0 | None | Never sent |
| 1 | InvalidQuantity | Quantity was zero |
| 2 | PriceOutOfBand | Limit price outside the symbol's band |
| 3 | NotFillable | A fill-or-kill could not fill in full |
| 4 | BookFull | No room for another resting order |
| 5 | UnknownOrder | Cancel of an order that is not resting, or is not this session's |
| 6 | UnknownSymbol | |
| 7 | Malformed | A field held a value outside its range |
| 8 | NotLoggedIn | |
| 9 | Busy | The venue is shedding load. Try again. |

### What to expect for one order

| You send | You get, in this order |
|----------|------------------------|
| Day limit that does not cross | `OrderAccepted` |
| Day limit that crosses in part | `OrderAccepted`, one `Executed` per fill. The rest stays on the book. |
| IOC or market, partly filled | `OrderAccepted`, `Executed` per fill, `Canceled` with reason 1 for the rest |
| FOK that cannot fill in full | `OrderRejected`, reason 3. Nothing trades. |
| Anything refused | `OrderRejected` only |
| Cancel of your resting order | `Canceled`, reason 0 |
| Cancel of anything else | `CancelRejected`, reason 5 |
| Nothing: your connection ends | `Canceled`, reason 2, for each order you had resting. Ask for them when you are back. |
| A `NewOrder` with an id you have used | Nothing |

The owner of a resting order that gets hit receives an `Executed` with
liquidity 0, with no warning before it.

## Market data (UDP)

One stream per shard. `planckd --feed HOST:PORT` sends shard N's stream to
PORT + N. The address may be a multicast group or a plain address.

### Datagram

Each datagram is a 16-byte packet header followed by `count` whole messages,
in the common message format. A datagram is at most 1400 bytes.

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 8 | `sequence` | Sequence number of the first message here. Each one after it is one higher. Starts at 1. |
| 8 | 2 | `count` | Messages in this datagram. Zero means heartbeat. |
| 10 | 2 | `stream_id` | The shard |
| 12 | 4 | reserved | |

A **heartbeat** has `count` zero, and `sequence` is the number the next real
message will carry. One is sent after a second of silence. If it names a
number higher than you expected, you missed something.

### Messages

**MdAddOrder**, type 0x40, 32 bytes. An order came to rest.

| Offset | Size | Field |
|-------:|-----:|-------|
| 4 | 2 | `symbol_id` |
| 6 | 1 | `side` (0 buy, 1 sell) |
| 7 | 1 | reserved |
| 8 | 8 | `order_id` |
| 16 | 4 | `price` |
| 20 | 4 | `qty` |
| 24 | 8 | `timestamp` |

**MdExecuted**, type 0x41, 40 bytes. A resting order traded.

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `symbol_id` | |
| 6 | 2 | reserved | |
| 8 | 8 | `order_id` | The resting order |
| 16 | 8 | `match_id` | |
| 24 | 4 | `qty` | Take this off the resting order. At zero it is gone. |
| 28 | 4 | `price` | |
| 32 | 8 | `timestamp` | |

Note that quantity comes before price here, the other way round from
`MdAddOrder`.

**MdDeleted**, type 0x42, 24 bytes. A resting order was cancelled.

| Offset | Size | Field |
|-------:|-----:|-------|
| 4 | 2 | `symbol_id` |
| 6 | 2 | reserved |
| 8 | 8 | `order_id` |
| 16 | 8 | `timestamp` |

The feed is order by order and anonymous. An order that trades in full on
arrival never appears as an add. Only the resting side of a trade is named.

### Building a book

Keep a table from `order_id` to symbol, side, price and remaining quantity.
Add on `MdAddOrder`, subtract on `MdExecuted` and remove at zero, remove on
`MdDeleted`. `tests/support/feed_auditor.hpp` does exactly this and is the
reference.

You must start from sequence 1, or from within the venue's retransmit
history. There are no snapshots (W-10, D-05).

### Recovering a gap

Send one 16-byte packet header, as a UDP datagram, to the venue's retransmit
port (`planckd --retransmit`, default 30000):

| Field | Set to |
|-------|--------|
| `sequence` | First message you are missing |
| `count` | How many you want. The venue sends at most 512 per request. |
| `stream_id` | The stream |

The venue sends the messages, in ordinary datagrams, to the address the
request came from. It answers nothing if the range is older than its
history (65,536 messages per stream by default) or not yet published. Ask
again for the rest. `feed::Receiver` is a complete implementation.

The retransmit port has no rate limit and answers anyone (W-11).

## Journal (disk)

One file per shard, `<dir>/shard-NN.journal`. A 64-byte header, then 48-byte
records until the end of the file. Checksums are CRC-32C (Castagnoli) over
all the bytes of the header or record before the checksum field.

### File header, 64 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 8 | `magic` | `PLNKJRNL` |
| 8 | 4 | `version` | 3. Versions 1 and 2 are refused (S-26, S-30). |
| 12 | 2 | `shard` | |
| 14 | 2 | `header_bytes` | 64 |
| 16 | 8 | `fingerprint` | Hash of the shard's configuration. Replay refuses a mismatch. |
| 24 | 8 | `created_ns` | |
| 32 | 28 | reserved | |
| 60 | 4 | `checksum` | |

### Record, 48 bytes

One command, as the shard handled it.

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 8 | `sequence` | Counts from 1 with no gaps |
| 8 | 8 | `timestamp_ns` | The time the shard gave the command |
| 16 | 8 | `id` | New order: the client's order id. Cancel: the venue's order id. |
| 24 | 4 | `price` | |
| 28 | 4 | `qty` | |
| 32 | 2 | `symbol` | |
| 34 | 2 | `session` | The owner: a client id |
| 36 | 1 | `kind` | 0 new order, 1 cancel, 2 session closed, 3 all sessions closed |
| 37 | 1 | `side` | |
| 38 | 1 | `type` | |
| 39 | 1 | `tif` | |
| 40 | 4 | reserved | |
| 44 | 4 | `checksum` | |

A "session closed" record uses only `session`, and "all sessions closed"
uses none of the fields after the timestamp. The unused ones are zero.
Neither says which orders were cancelled: replaying the journal up to that
point shows what was resting.

### Reading one

- A last record that is short or fails its checksum is a **torn tail**: the
  mark of a crash. Drop it.
- A bad record with good records after it is **damage**. Stop. History is
  missing and a person should decide (S-13).

`planck-replay --file F --dump` prints a journal and exits 0 for intact, 3
for a torn tail, 4 for damage and 1 for unreadable.

## Changing the protocol

Any change to a layout here is a protocol change. The golden-bytes tests in
`tests/unit/wire_test.cpp` and the fixture files checked by
`tests/unit/wire_fixture_test.cpp` will fail, on purpose. So is a change to what a
field means, which no test can notice: that is why the journal went to
version 2 when its session field changed meaning. A new kind of record is
a change too, and took it to version 3. Then:

1. Bump `wire::kProtocolVersion` or `journal::kVersion`.
2. Update the `static_assert`s, the golden bytes and this document together.
3. For the wire protocol, make the same change in `rust/planck-wire/`: the
   table in its `message!` declaration, `PROTOCOL_VERSION`, and the crate's
   minor version, which is the protocol version. Then write the fixture
   files again, into a new `tests/fixtures/wire/v<N>/`, and repeat the new
   values in `rust/planck-wire/tests/golden.rs`. See "Other languages".
4. For the journal, decide what happens to old files. There is no migration
   tool (W-74).
5. If the C library's structs change, bump `PLANCK_ABI_VERSION` too.

The order entry protocol went to version 2 when heartbeats stopped being
answered and `LoginAccepted` gained the idle timeout. The second of those
used bytes that were reserved, so no size changed. The first changed what
a correct client does, and that is a new version however small it looks.

It went to version 3 when reports were numbered. `LoginRequest` and
`LoginAccepted` both grew to 40 bytes, and a client order id stopped being
the client's own business: the venue now reads it.

## Other languages

The protocol is the contract, not the C++ headers. A program in another
language needs its own codec, and a way to know that codec is right.

### The fixture files

`tests/fixtures/wire/v3/` holds the protocol as bytes, in 18 files:

| Files | What each holds |
|-------|-----------------|
| `login_request.bin` and 14 more, one per message | One message, every field set to a value that could not be mistaken for its neighbour |
| `order_entry_stream.bin` | Five messages back to back, as a client reads them off a connection: `LoginAccepted`, `OrderAccepted`, `Executed`, `Heartbeat`, `Canceled` |
| `md_packet.bin` | One datagram: a packet header with sequence 1000, then `MdAddOrder`, `MdExecuted` and `MdDeleted` |
| `md_heartbeat.bin` | One datagram with no messages |

`tests/unit/wire_fixture_test.cpp` builds each of them from the C++ structs
on every test run and fails if a file differs. So the files are always what
the venue really sends. A codec in any language that decodes every file to
the right values, and encodes those values back to the same bytes, speaks
the protocol.

To write them again after a deliberate change:

```bash
PLANCK_WRITE_FIXTURES=1 build/gcc-checked/tests/planck_unit_tests \
    --gtest_filter='WireFixtures.*'
```

The files are per protocol version. Keep the old directory when the
version changes, so that a reader of old captures can still be tested.

### Rust: `rust/planck-wire/`

A crate with no dependencies and no `unsafe`. It is a codec and nothing
more: no sockets, no sessions, no timers.

| Rust | C++ |
|------|-----|
| `next_frame(buffer)` | `wire::next_frame` |
| `T::decode(bytes)`, `T::encode(out)`, `T::to_bytes()` | `wire::decode<T>`, `wire::encode<T>` |
| `VenueMessage::decode(frame)` | a `switch` on `frame.type` |
| `Packet::parse(datagram)`, `packet.messages()` | the loop in `feed::Receiver` |
| `MsgType::is_report()` | `wire::is_report` |

Two differences from the C++ side, both on purpose:

- **`decode` is the whole trust boundary.** In C++ `decode` checks the
  shape and `well_formed` checks the values, and only for the three
  messages a client sends. In Rust an enum cannot hold a value outside its
  range, so `decode` checks every enum and every reserved byte of every
  message, and there is no `well_formed`. A Rust program is therefore
  stricter about what the venue sends it than the C++ client is.
- **`Packet::parse` is all or nothing.** It decodes every message in a
  datagram before it hands out the first, so a damaged datagram changes
  nothing in the book being built.

To use it from another repository:

```toml
[dependencies]
planck-wire = { git = "https://github.com/tembolo1284/planck" }
```

Cargo finds the crate by name inside the repository. Pin a `rev` once a
consumer depends on a particular protocol version.

The crate's tests read the fixture files above, and one of them fails if
the directory holds a file the suite has no test for. So a message added
in C++ cannot be forgotten in Rust. They run as the `rust_wire` test
wherever `cargo` is installed. See [testing.md](testing.md).
