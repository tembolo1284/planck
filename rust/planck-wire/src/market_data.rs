//! Market data, over UDP. One stream per matching shard.
//!
//! An order-by-order feed. A subscriber that applies every message in
//! sequence holds an exact copy of each book.
//!
//! - [`MdAddOrder`]: an order started resting.
//! - [`MdExecuted`]: a resting order traded, in part or in full.
//! - [`MdDeleted`]: a resting order was cancelled.
//!
//! An order whose open quantity reaches zero through `MdExecuted` is
//! gone. No `MdDeleted` follows. Orders that trade without ever resting
//! appear only as the `MdExecuted` of whatever they hit.
//!
//! A datagram is one [`PacketHeader`] followed by `count` messages back
//! to back. [`Packet::parse`] checks a whole datagram before it hands
//! out any of it, so a damaged one changes nothing.

use crate::codec::{Message, message};
use crate::error::WireError;
use crate::frame::{Frame, next_frame};
use crate::types::{MsgType, Side};

message! {
    MdAddOrder: 32 {
        4 => symbol_id: u16,
        6 => side: Side,
        8 => order_id: u64,
        16 => price: i32,
        20 => qty: u32,
        24 => timestamp: u64,
    }
}

message! {
    MdExecuted: 40 {
        4 => symbol_id: u16,
        /// The resting order that was hit.
        8 => order_id: u64,
        16 => match_id: u64,
        /// Quantity traded. Subtract it from the order's open quantity.
        24 => qty: u32,
        /// Always the resting order's price.
        28 => price: i32,
        32 => timestamp: u64,
    }
}

message! {
    MdDeleted: 24 {
        4 => symbol_id: u16,
        8 => order_id: u64,
        16 => timestamp: u64,
    }
}

/// One message of the feed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MdMessage {
    AddOrder(MdAddOrder),
    Executed(MdExecuted),
    Deleted(MdDeleted),
}

impl MdMessage {
    /// Read the message in a frame from [`crate::next_frame`].
    ///
    /// # Errors
    ///
    /// [`WireError::BadPacket`] for a message that is not market data.
    /// Otherwise whatever the message's own `decode` returns.
    pub fn decode(frame: Frame<'_>) -> Result<Self, WireError> {
        match frame.msg_type {
            MsgType::MdAddOrder => MdAddOrder::decode(frame.bytes).map(Self::AddOrder),
            MsgType::MdExecuted => MdExecuted::decode(frame.bytes).map(Self::Executed),
            MsgType::MdDeleted => MdDeleted::decode(frame.bytes).map(Self::Deleted),
            _ => Err(WireError::BadPacket("not a market data message")),
        }
    }

    #[must_use]
    pub const fn symbol_id(&self) -> u16 {
        match self {
            Self::AddOrder(message) => message.symbol_id,
            Self::Executed(message) => message.symbol_id,
            Self::Deleted(message) => message.symbol_id,
        }
    }

    #[must_use]
    pub const fn order_id(&self) -> u64 {
        match self {
            Self::AddOrder(message) => message.order_id,
            Self::Executed(message) => message.order_id,
            Self::Deleted(message) => message.order_id,
        }
    }

    #[must_use]
    pub const fn timestamp(&self) -> u64 {
        match self {
            Self::AddOrder(message) => message.timestamp,
            Self::Executed(message) => message.timestamp,
            Self::Deleted(message) => message.timestamp,
        }
    }
}

/// The first 16 bytes of every datagram. Sent alone, back to the venue,
/// it is also the request for a retransmission.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PacketHeader {
    /// Sequence number of the first message in the datagram. Each one
    /// after it is one higher. Starts at 1.
    pub sequence: u64,
    /// Messages in the datagram. Zero is a heartbeat: `sequence` is then
    /// the number the next real message will carry.
    pub count: u16,
    /// Which shard's stream this is.
    pub stream_id: u16,
}

impl PacketHeader {
    pub const SIZE: usize = 16;

    #[must_use]
    pub fn to_bytes(&self) -> [u8; Self::SIZE] {
        let mut out = [0u8; Self::SIZE];
        out[..8].copy_from_slice(&self.sequence.to_le_bytes());
        out[8..10].copy_from_slice(&self.count.to_le_bytes());
        out[10..12].copy_from_slice(&self.stream_id.to_le_bytes());
        out
    }

    /// Read the header at the front of a datagram.
    ///
    /// # Errors
    ///
    /// `Truncated` if there are fewer than 16 bytes, `BadPacket` if the
    /// reserved bytes are not zero.
    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let Some(header) = bytes.first_chunk::<{ Self::SIZE }>() else {
            return Err(WireError::Truncated {
                needed: Self::SIZE,
                got: bytes.len(),
            });
        };
        let [
            s0,
            s1,
            s2,
            s3,
            s4,
            s5,
            s6,
            s7,
            c0,
            c1,
            i0,
            i1,
            reserved @ ..,
        ] = *header;
        if reserved != [0; 4] {
            return Err(WireError::BadPacket("reserved bytes are not zero"));
        }
        Ok(Self {
            sequence: u64::from_le_bytes([s0, s1, s2, s3, s4, s5, s6, s7]),
            count: u16::from_le_bytes([c0, c1]),
            stream_id: u16::from_le_bytes([i0, i1]),
        })
    }

    #[must_use]
    pub const fn is_heartbeat(&self) -> bool {
        self.count == 0
    }

    /// One past the last sequence number this datagram proves to exist.
    /// The next datagram of the stream starts here unless one was lost.
    #[must_use]
    pub const fn next_sequence(&self) -> u64 {
        self.sequence.saturating_add(self.count as u64)
    }
}

/// A datagram that has been checked from end to end.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Packet<'a> {
    pub header: PacketHeader,
    body: &'a [u8],
}

impl<'a> Packet<'a> {
    /// Check a whole datagram.
    ///
    /// Sequence numbers, the stream id and gaps are the caller's
    /// business: this proves only that the bytes are what they claim.
    ///
    /// # Errors
    ///
    /// Any error of [`PacketHeader::decode`] or of a message's `decode`,
    /// and `BadPacket` if the body is not exactly `count` whole market
    /// data messages or the sequence numbers would overflow.
    pub fn parse(datagram: &'a [u8]) -> Result<Self, WireError> {
        let header = PacketHeader::decode(datagram)?;
        if header
            .sequence
            .checked_add(u64::from(header.count))
            .is_none()
        {
            return Err(WireError::BadPacket("sequence numbers overflow"));
        }
        let body = &datagram[PacketHeader::SIZE..];
        let mut rest = body;
        for _ in 0..header.count {
            let Some(frame) = next_frame(rest)? else {
                return Err(WireError::BadPacket("fewer messages than the header says"));
            };
            MdMessage::decode(frame)?;
            rest = &rest[frame.bytes.len()..];
        }
        if !rest.is_empty() {
            return Err(WireError::BadPacket("bytes after the last message"));
        }
        Ok(Self { header, body })
    }

    /// The messages in order, each with its sequence number.
    #[must_use]
    pub const fn messages(&self) -> Messages<'a> {
        Messages {
            sequence: self.header.sequence,
            rest: self.body,
        }
    }
}

/// Iterator over the messages of a [`Packet`].
#[derive(Debug, Clone)]
pub struct Messages<'a> {
    sequence: u64,
    rest: &'a [u8],
}

impl Iterator for Messages<'_> {
    /// Sequence number and message.
    type Item = (u64, MdMessage);

    fn next(&mut self) -> Option<Self::Item> {
        // `Packet::parse` has already decoded every message once, so
        // neither step can fail here. Ending early is the answer that
        // needs no panic if that ever stops being true.
        let frame = next_frame(self.rest).ok()??;
        let message = MdMessage::decode(frame).ok()?;
        let sequence = self.sequence;
        self.rest = &self.rest[frame.bytes.len()..];
        self.sequence += 1;
        Some((sequence, message))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::order_entry::Heartbeat;

    fn add(order_id: u64) -> MdAddOrder {
        MdAddOrder {
            symbol_id: 1,
            side: Side::Buy,
            order_id,
            price: 100,
            qty: 10,
            timestamp: 5,
        }
    }

    fn datagram(header: PacketHeader, messages: &[&[u8]]) -> Vec<u8> {
        let mut out = header.to_bytes().to_vec();
        for message in messages {
            out.extend_from_slice(message);
        }
        out
    }

    #[test]
    fn messages_carry_consecutive_sequence_numbers() {
        let header = PacketHeader {
            sequence: 50,
            count: 2,
            stream_id: 0,
        };
        let bytes = datagram(header, &[&add(1).to_bytes(), &add(2).to_bytes()]);
        let packet = Packet::parse(&bytes).unwrap();
        let seen: Vec<_> = packet.messages().collect();
        assert_eq!(
            seen,
            vec![
                (50, MdMessage::AddOrder(add(1))),
                (51, MdMessage::AddOrder(add(2)))
            ]
        );
        assert_eq!(packet.header.next_sequence(), 52);
    }

    #[test]
    fn count_and_body_must_agree() {
        let two = PacketHeader {
            sequence: 1,
            count: 2,
            stream_id: 0,
        };
        let short = datagram(two, &[&add(1).to_bytes()]);
        assert!(matches!(
            Packet::parse(&short),
            Err(WireError::BadPacket(_))
        ));

        let one = PacketHeader { count: 1, ..two };
        let long = datagram(one, &[&add(1).to_bytes(), &add(2).to_bytes()]);
        assert!(matches!(Packet::parse(&long), Err(WireError::BadPacket(_))));

        let cut = &long[..long.len() - 40];
        assert!(matches!(Packet::parse(cut), Err(WireError::BadPacket(_))));
    }

    #[test]
    fn only_market_data_belongs_in_a_datagram() {
        let header = PacketHeader {
            sequence: 1,
            count: 1,
            stream_id: 0,
        };
        let bytes = datagram(header, &[&Heartbeat {}.to_bytes()]);
        assert!(matches!(
            Packet::parse(&bytes),
            Err(WireError::BadPacket(_))
        ));
    }

    #[test]
    fn a_damaged_message_spoils_the_whole_datagram() {
        let header = PacketHeader {
            sequence: 1,
            count: 2,
            stream_id: 0,
        };
        let mut bytes = datagram(header, &[&add(1).to_bytes(), &add(2).to_bytes()]);
        // The side of the second message.
        bytes[PacketHeader::SIZE + 32 + 6] = 9;
        assert!(matches!(
            Packet::parse(&bytes),
            Err(WireError::BadField { field: "side", .. })
        ));
    }

    #[test]
    fn header_checks() {
        assert!(matches!(
            PacketHeader::decode(&[0; 15]),
            Err(WireError::Truncated {
                needed: 16,
                got: 15
            })
        ));
        let mut bytes = PacketHeader {
            sequence: 1,
            count: 0,
            stream_id: 0,
        }
        .to_bytes();
        bytes[15] = 1;
        assert!(matches!(
            PacketHeader::decode(&bytes),
            Err(WireError::BadPacket(_))
        ));
        let overflow = PacketHeader {
            sequence: u64::MAX,
            count: 1,
            stream_id: 0,
        };
        assert!(matches!(
            Packet::parse(&datagram(overflow, &[&add(1).to_bytes()])),
            Err(WireError::BadPacket(_))
        ));
    }
}
