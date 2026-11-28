//! The planck wire protocol in Rust.
//!
//! A second implementation of `include/planck/wire/`, for programs that
//! talk to a planck venue without linking any C++: a feed handler, an
//! order entry client, a drop copy reader. `docs/protocol.md` is the
//! specification.
//!
//! Every message is a fixed-size little-endian struct that starts with a
//! four-byte header. Each one here has `to_bytes`, and `encode` and
//! `decode` through [`Message`]. `decode` is the trust boundary: it
//! checks the header, every enum and every reserved byte, so a value that
//! exists is one the rest of a program may rely on.
//!
//! - [`order_entry`] is the TCP side. Cut the byte stream into messages
//!   with [`next_frame`], then [`VenueMessage::decode`].
//! - [`market_data`] is the UDP side. Hand each datagram to
//!   [`Packet::parse`].
//!
//! Nothing here allocates, does I/O or uses `unsafe`.
//!
//! The tests in `tests/golden.rs` read the files under
//! `tests/fixtures/wire/` at the top of the repository. The C++ test
//! `tests/unit/wire_fixture_test.cpp` checks the same files, so the two
//! codecs cannot drift apart without one of them failing.

#![forbid(unsafe_code)]

mod codec;
pub mod error;
pub mod frame;
pub mod market_data;
pub mod order_entry;
pub mod types;

pub use codec::Message;
pub use error::WireError;
pub use frame::{Frame, next_frame};
pub use market_data::{MdAddOrder, MdDeleted, MdExecuted, MdMessage, Packet, PacketHeader};
pub use order_entry::{
    CancelOrder, CancelRejected, Canceled, Executed, Heartbeat, LoginAccepted, LoginRejected,
    LoginRequest, Logout, NewOrder, OrderAccepted, OrderRejected, VenueMessage,
};
pub use types::{
    CancelReason, Liquidity, LoginRejectReason, MsgType, OrderType, RejectCode, Side, TimeInForce,
};

/// `wire::kProtocolVersion`. A venue refuses a login that names another.
pub const PROTOCOL_VERSION: u16 = 3;

/// Size of the header every message starts with.
pub const HEADER_SIZE: usize = 4;

/// The largest message. A buffer of this size holds any one of them.
pub const MAX_MESSAGE_SIZE: usize = 48;

/// The largest market data datagram a venue sends.
pub const MAX_DATAGRAM_SIZE: usize = 1400;
