//! Order entry, over TCP. One session per connection.
//!
//! The client logs in, then sends [`NewOrder`] and [`CancelOrder`]. The
//! venue answers every `NewOrder` with exactly one [`OrderAccepted`] or
//! [`OrderRejected`], then zero or more [`Executed`], and a [`Canceled`]
//! if the order leaves the book unfilled.
//!
//! The venue numbers the reports it sends a session, from 1, across
//! however many connections the session uses. The numbers are not on the
//! wire: [`LoginAccepted`] says what number the next report has and the
//! client counts from there, using [`VenueMessage::is_report`].
//!
//! Prices are ticks. Timestamps are nanoseconds since the Unix epoch.

use crate::PROTOCOL_VERSION;
use crate::codec::{Message, message};
use crate::error::WireError;
use crate::frame::Frame;
use crate::types::{
    CancelReason, Liquidity, LoginRejectReason, MsgType, OrderType, RejectCode, Side, TimeInForce,
};

// ---- session ----

message! {
    /// Client to venue. The first message on every connection.
    LoginRequest: 40 {
        4 => protocol_version: u16,
        8 => client_id: u32,
        16 => token: u64,
        /// The epoch from the `LoginAccepted` of an earlier connection.
        /// Zero if there was none.
        24 => epoch: u64,
        /// The number of the first report the client has not seen. Zero:
        /// only what is new. 1: everything the venue still holds.
        32 => next_report: u64,
    }
}

impl LoginRequest {
    /// A first login: this protocol version, and nothing wanted from
    /// before.
    #[must_use]
    pub const fn fresh(client_id: u32, token: u64) -> Self {
        Self {
            protocol_version: PROTOCOL_VERSION,
            client_id,
            token,
            epoch: 0,
            next_report: 0,
        }
    }

    /// A login after a lost connection: ask for every report from
    /// `next_report` on, counted in the venue run named by `epoch`.
    #[must_use]
    pub const fn resuming(client_id: u32, token: u64, epoch: u64, next_report: u64) -> Self {
        Self {
            protocol_version: PROTOCOL_VERSION,
            client_id,
            token,
            epoch,
            next_report,
        }
    }
}

message! {
    /// Venue to client.
    LoginAccepted: 40 {
        4 => session_id: u16,
        /// The venue ends a connection it has heard nothing on for this
        /// long, in milliseconds. Zero means it never does. Send
        /// something, a `Heartbeat` if nothing else, about three times as
        /// often.
        6 => idle_timeout_ms: u16,
        /// This run of the venue. Report numbers start again at 1 when
        /// it changes.
        8 => epoch: u64,
        /// The number of the first report this connection will carry.
        16 => next_report: u64,
        /// The number the first new report will have. Those from
        /// `next_report` up to here are from before, sent again.
        24 => live_from: u64,
        /// The highest client order id the venue has had from this
        /// session. A `NewOrder` whose id is not above it is ignored.
        32 => last_client_order_id: u64,
    }
}

message! {
    /// Venue to client. The connection closes after it.
    LoginRejected: 8 {
        4 => reason: LoginRejectReason,
    }
}

message! {
    /// Either way, when there has been nothing else to say. Not answered.
    Heartbeat: 4 {}
}

message! {
    /// Either way.
    Logout: 4 {}
}

// ---- client to venue ----

message! {
    NewOrder: 32 {
        4 => symbol_id: u16,
        6 => side: Side,
        7 => order_type: OrderType,
        /// Chosen by the client and echoed back. It must be above every
        /// id the session has sent before, on any connection. An order
        /// whose id is not is taken for a repeat and ignored without an
        /// answer, which makes sending an order again safe.
        8 => client_order_id: u64,
        /// Ticks. Ignored for market orders.
        16 => price: i32,
        20 => qty: u32,
        24 => tif: TimeInForce,
    }
}

message! {
    /// Cancels by the venue's order id from `OrderAccepted`. There is no
    /// cancel by client order id.
    CancelOrder: 16 {
        8 => order_id: u64,
    }
}

// ---- venue to client ----

message! {
    OrderAccepted: 32 {
        8 => client_order_id: u64,
        /// The venue's id for this order. Use it to cancel.
        16 => order_id: u64,
        24 => timestamp: u64,
    }
}

message! {
    OrderRejected: 16 {
        4 => reason: RejectCode,
        8 => client_order_id: u64,
    }
}

message! {
    /// One fill. Sent to both parties of the trade, each with its own
    /// `order_id` and liquidity flag and the same `match_id`.
    Executed: 48 {
        4 => liquidity: Liquidity,
        8 => order_id: u64,
        16 => match_id: u64,
        24 => price: i32,
        28 => qty: u32,
        /// Open quantity after this fill. Zero means the order is done.
        32 => leaves: u32,
        40 => timestamp: u64,
    }
}

message! {
    Canceled: 32 {
        4 => reason: CancelReason,
        8 => order_id: u64,
        /// Quantity removed from the book.
        16 => qty: u32,
        24 => timestamp: u64,
    }
}

message! {
    CancelRejected: 16 {
        4 => reason: RejectCode,
        8 => order_id: u64,
    }
}

/// Anything a venue sends a client on an order entry connection.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VenueMessage {
    LoginAccepted(LoginAccepted),
    LoginRejected(LoginRejected),
    Heartbeat,
    Logout,
    OrderAccepted(OrderAccepted),
    OrderRejected(OrderRejected),
    Executed(Executed),
    Canceled(Canceled),
    CancelRejected(CancelRejected),
}

impl VenueMessage {
    /// Read the message in a frame from [`crate::next_frame`].
    ///
    /// # Errors
    ///
    /// [`WireError::WrongType`] for a message only a client sends or one
    /// from the market data feed, with `expected` set to `Heartbeat` for
    /// want of a better answer. Otherwise whatever the message's own
    /// `decode` returns.
    pub fn decode(frame: Frame<'_>) -> Result<Self, WireError> {
        let bytes = frame.bytes;
        Ok(match frame.msg_type {
            MsgType::LoginAccepted => Self::LoginAccepted(LoginAccepted::decode(bytes)?),
            MsgType::LoginRejected => Self::LoginRejected(LoginRejected::decode(bytes)?),
            MsgType::Heartbeat => Heartbeat::decode(bytes).map(|_| Self::Heartbeat)?,
            MsgType::Logout => Logout::decode(bytes).map(|_| Self::Logout)?,
            MsgType::OrderAccepted => Self::OrderAccepted(OrderAccepted::decode(bytes)?),
            MsgType::OrderRejected => Self::OrderRejected(OrderRejected::decode(bytes)?),
            MsgType::Executed => Self::Executed(Executed::decode(bytes)?),
            MsgType::Canceled => Self::Canceled(Canceled::decode(bytes)?),
            MsgType::CancelRejected => Self::CancelRejected(CancelRejected::decode(bytes)?),
            got @ (MsgType::LoginRequest
            | MsgType::NewOrder
            | MsgType::CancelOrder
            | MsgType::MdAddOrder
            | MsgType::MdExecuted
            | MsgType::MdDeleted) => {
                return Err(WireError::WrongType {
                    expected: MsgType::Heartbeat,
                    got,
                });
            }
        })
    }

    #[must_use]
    pub const fn msg_type(&self) -> MsgType {
        match self {
            Self::LoginAccepted(_) => MsgType::LoginAccepted,
            Self::LoginRejected(_) => MsgType::LoginRejected,
            Self::Heartbeat => MsgType::Heartbeat,
            Self::Logout => MsgType::Logout,
            Self::OrderAccepted(_) => MsgType::OrderAccepted,
            Self::OrderRejected(_) => MsgType::OrderRejected,
            Self::Executed(_) => MsgType::Executed,
            Self::Canceled(_) => MsgType::Canceled,
            Self::CancelRejected(_) => MsgType::CancelRejected,
        }
    }

    /// True if the venue numbered this message. Add one to the report
    /// count for each of these and for nothing else.
    #[must_use]
    pub const fn is_report(&self) -> bool {
        self.msg_type().is_report()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::next_frame;

    fn order() -> NewOrder {
        NewOrder {
            symbol_id: 3,
            side: Side::Buy,
            order_type: OrderType::Limit,
            client_order_id: 10,
            price: 100,
            qty: 5,
            tif: TimeInForce::Day,
        }
    }

    #[test]
    fn encode_needs_room_for_the_whole_message() {
        let mut small = [0u8; 31];
        assert_eq!(
            order().encode(&mut small),
            Err(WireError::BufferTooSmall {
                needed: 32,
                got: 31
            })
        );
        let mut exact = [0xFFu8; 32];
        assert_eq!(order().encode(&mut exact), Ok(32));
        assert_eq!(NewOrder::decode(&exact), Ok(order()));
    }

    #[test]
    fn decode_reads_the_front_and_ignores_what_follows() {
        let mut bytes = order().to_bytes().to_vec();
        bytes.extend_from_slice(&[0xAA; 8]);
        assert_eq!(NewOrder::decode(&bytes), Ok(order()));
    }

    #[test]
    fn decode_refuses_a_short_message() {
        let bytes = order().to_bytes();
        assert_eq!(
            NewOrder::decode(&bytes[..31]),
            Err(WireError::Truncated {
                needed: 32,
                got: 31
            })
        );
        assert!(matches!(
            NewOrder::decode(&bytes[..3]),
            Err(WireError::Truncated { .. })
        ));
    }

    #[test]
    fn decode_refuses_another_message() {
        let bytes = CancelOrder { order_id: 1 }.to_bytes();
        assert_eq!(
            NewOrder::decode(&bytes),
            Err(WireError::WrongType {
                expected: MsgType::NewOrder,
                got: MsgType::CancelOrder
            })
        );
    }

    #[test]
    fn decode_refuses_an_enum_out_of_range() {
        let mut bytes = order().to_bytes();
        bytes[6] = 2;
        assert_eq!(
            NewOrder::decode(&bytes),
            Err(WireError::BadField {
                message: MsgType::NewOrder,
                field: "side"
            })
        );
        let mut bytes = order().to_bytes();
        bytes[24] = 3;
        assert_eq!(
            NewOrder::decode(&bytes),
            Err(WireError::BadField {
                message: MsgType::NewOrder,
                field: "tif"
            })
        );
    }

    #[test]
    fn decode_refuses_any_reserved_byte_that_is_set() {
        let good = order().to_bytes();
        for index in 25..32 {
            let mut bytes = good;
            bytes[index] = 1;
            assert_eq!(
                NewOrder::decode(&bytes),
                Err(WireError::ReservedNotZero {
                    message: MsgType::NewOrder
                }),
                "byte {index}"
            );
        }
        let mut login = LoginRequest::fresh(1, 2).to_bytes();
        login[6] = 1;
        assert!(matches!(
            LoginRequest::decode(&login),
            Err(WireError::ReservedNotZero { .. })
        ));
    }

    #[test]
    fn a_venue_never_sends_what_only_a_client_sends() {
        let bytes = order().to_bytes();
        let frame = next_frame(&bytes).unwrap().unwrap();
        assert!(matches!(
            VenueMessage::decode(frame),
            Err(WireError::WrongType {
                got: MsgType::NewOrder,
                ..
            })
        ));
    }

    #[test]
    fn login_helpers_name_this_protocol_version() {
        assert_eq!(LoginRequest::fresh(1, 2).protocol_version, PROTOCOL_VERSION);
        let again = LoginRequest::resuming(1, 2, 3, 4);
        assert_eq!((again.epoch, again.next_report), (3, 4));
    }
}
