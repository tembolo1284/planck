# Protocol

Everything planck puts on a wire or a disk, byte by byte. Enough to write an
order entry client, a feed handler or a journal reader without reading the
source. The source of truth is `include/planck/wire/` and
`include/planck/journal/format.hpp`, where `static_assert`s pin every size
and offset given here.

## Common rules

- Protocol version 1.
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

### Session

1. Client connects and sends `LoginRequest`.
2. Venue answers `LoginAccepted` with a session id, or `LoginRejected` and
   closes.
3. Client sends `NewOrder` and `CancelOrder`. Venue sends reports.
4. Client sends `Logout` or just closes.

An order or cancel before step 2 is answered with `OrderRejected` or
`CancelRejected`, reason NotLoggedIn. A `Heartbeat` from the client is echoed. The venue sends none
of its own and never times a client out (W-03).

A session id is not reused. A client that reconnects gets a new one and can
no longer cancel orders from the old session (D-01).

### Client to venue

**LoginRequest**, type 0x01, 24 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `protocol_version` | 1 |
| 6 | 2 | reserved | |
| 8 | 4 | `client_id` | Who you are. One login per client id at a time. |
| 12 | 4 | reserved | |
| 16 | 8 | `token` | Accepted and ignored today (D-02) |

**Heartbeat**, type 0x04, 4 bytes. Header only. Echoed.

**Logout**, type 0x05, 4 bytes. Header only.

**NewOrder**, type 0x10, 32 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 2 | `symbol_id` | |
| 6 | 1 | `side` | 0 buy, 1 sell |
| 7 | 1 | `order_type` | 0 limit, 1 market |
| 8 | 8 | `client_order_id` | Yours. Echoed in the answer, never interpreted. |
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

**LoginAccepted**, type 0x02, 8 bytes

| Offset | Size | Field |
|-------:|-----:|-------|
| 4 | 2 | `session_id` |
| 6 | 2 | reserved |

**LoginRejected**, type 0x03, 8 bytes

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 4 | 1 | `reason` | 0 bad credentials, 1 already logged in, 2 unsupported version, 3 server full |
| 5 | 3 | reserved | |

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
| 4 | 1 | `reason` | 0 you asked, 1 unfilled rest of an IOC or market order, 2 disconnected (defined, not sent today) |
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
| 8 | 4 | `version` | 1 |
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
| 34 | 2 | `session` | |
| 36 | 1 | `kind` | 0 new order, 1 cancel |
| 37 | 1 | `side` | |
| 38 | 1 | `type` | |
| 39 | 1 | `tif` | |
| 40 | 4 | reserved | |
| 44 | 4 | `checksum` | |

### Reading one

- A last record that is short or fails its checksum is a **torn tail**: the
  mark of a crash. Drop it.
- A bad record with good records after it is **damage**. Stop. History is
  missing and a person should decide (S-13).

`planck-replay --file F --dump` prints a journal and exits 0 for intact, 3
for a torn tail, 4 for damage and 1 for unreadable.

## Changing the protocol

Any change to a layout here is a protocol change. The golden-bytes tests in
`tests/unit/wire_test.cpp` and `journal_test.cpp` will fail, on purpose.
Then:

1. Bump `wire::kProtocolVersion` or `journal::kVersion`.
2. Update the `static_assert`s, the golden bytes and this document together.
3. For the journal, decide what happens to old files. There is no migration
   tool (W-74).
4. If the C library's structs change, bump `PLANCK_ABI_VERSION` too.
