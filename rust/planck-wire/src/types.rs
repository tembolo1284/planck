//! The header's type codes and the enums that travel as single bytes.
//!
//! The numbers are those of `include/planck/wire/types.hpp` and
//! `include/planck/core/types.hpp`.

use crate::codec::Field;

/// Declare an enum that is one byte on the wire.
macro_rules! wire_enum {
    (
        $(#[$meta:meta])*
        $name:ident {
            $( $(#[$variant_meta:meta])* $variant:ident = $value:literal ),+ $(,)?
        }
    ) => {
        $(#[$meta])*
        #[repr(u8)]
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub enum $name {
            $( $(#[$variant_meta])* $variant = $value, )+
        }

        impl $name {
            /// The value for this byte, if it is one of the enum's.
            #[must_use]
            pub const fn from_u8(value: u8) -> Option<Self> {
                match value {
                    $( $value => Some(Self::$variant), )+
                    _ => None,
                }
            }

            /// The byte that goes on the wire.
            #[must_use]
            pub const fn as_u8(self) -> u8 {
                self as u8
            }

            /// The name, as the C++ `to_string` gives it.
            #[must_use]
            pub const fn name(self) -> &'static str {
                match self {
                    $( Self::$variant => stringify!($variant), )+
                }
            }
        }

        impl Field for $name {
            const SIZE: usize = 1;

            fn put(self, out: &mut [u8]) {
                out[0] = self.as_u8();
            }

            fn get(bytes: &[u8]) -> Option<Self> {
                Self::from_u8(*bytes.first()?)
            }
        }
    };
}

wire_enum! {
    /// The third byte of every message: which message it is.
    MsgType {
        /// Client to venue.
        LoginRequest = 0x01,
        /// Venue to client.
        LoginAccepted = 0x02,
        /// Venue to client.
        LoginRejected = 0x03,
        /// Either way. Nobody answers one.
        Heartbeat = 0x04,
        /// Either way.
        Logout = 0x05,
        /// Client to venue.
        NewOrder = 0x10,
        /// Client to venue.
        CancelOrder = 0x11,
        /// Venue to client. A report.
        OrderAccepted = 0x20,
        /// Venue to client. A report.
        OrderRejected = 0x21,
        /// Venue to client. A report.
        Executed = 0x22,
        /// Venue to client. A report.
        Canceled = 0x23,
        /// Venue to client. A report.
        CancelRejected = 0x24,
        /// Market data.
        MdAddOrder = 0x40,
        /// Market data.
        MdExecuted = 0x41,
        /// Market data.
        MdDeleted = 0x42,
    }
}

impl MsgType {
    /// Size in bytes of a message of this type, header included.
    #[must_use]
    pub const fn size(self) -> usize {
        match self {
            Self::Heartbeat | Self::Logout => 4,
            Self::LoginRejected => 8,
            Self::CancelOrder | Self::OrderRejected | Self::CancelRejected => 16,
            Self::MdDeleted => 24,
            Self::NewOrder | Self::OrderAccepted | Self::Canceled | Self::MdAddOrder => 32,
            Self::LoginRequest | Self::LoginAccepted | Self::MdExecuted => 40,
            Self::Executed => 48,
        }
    }

    /// True for the messages a venue numbers for a session. A client
    /// counts these, and nothing else, to know which report is which.
    #[must_use]
    pub const fn is_report(self) -> bool {
        matches!(
            self,
            Self::OrderAccepted
                | Self::OrderRejected
                | Self::Executed
                | Self::Canceled
                | Self::CancelRejected
        )
    }

    /// True for the three messages of the market data feed.
    #[must_use]
    pub const fn is_market_data(self) -> bool {
        matches!(self, Self::MdAddOrder | Self::MdExecuted | Self::MdDeleted)
    }
}

wire_enum! {
    /// `core::Side`.
    Side {
        Buy = 0,
        Sell = 1,
    }
}

wire_enum! {
    /// `core::OrderType`.
    OrderType {
        Limit = 0,
        Market = 1,
    }
}

wire_enum! {
    /// `core::TimeInForce`.
    TimeInForce {
        Day = 0,
        ImmediateOrCancel = 1,
        FillOrKill = 2,
    }
}

wire_enum! {
    /// Why an order or a cancel was refused.
    RejectCode {
        None = 0,
        InvalidQuantity = 1,
        PriceOutOfBand = 2,
        NotFillable = 3,
        BookFull = 4,
        UnknownOrder = 5,
        UnknownSymbol = 6,
        /// A field held a value outside its range.
        Malformed = 7,
        NotLoggedIn = 8,
        /// The venue is shedding load. Try again.
        Busy = 9,
    }
}

wire_enum! {
    /// Why an order left the book without being fully filled.
    CancelReason {
        UserRequested = 0,
        /// The remainder of an IOC or market order.
        Unfilled = 1,
        /// The owning session dropped.
        Disconnected = 2,
    }
}

wire_enum! {
    /// Which side of a fill the recipient was on.
    Liquidity {
        /// The order was resting.
        Maker = 0,
        /// The order crossed the spread.
        Taker = 1,
    }
}

wire_enum! {
    /// Why a login was refused.
    LoginRejectReason {
        BadCredentials = 0,
        AlreadyLoggedIn = 1,
        UnsupportedVersion = 2,
        ServerFull = 3,
        /// The client asked for reports from a number the venue has not
        /// reached.
        BadSequence = 4,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::MAX_MESSAGE_SIZE;

    #[test]
    fn every_type_code_round_trips_and_nothing_else_does() {
        let mut known = 0;
        for value in 0..=u8::MAX {
            if let Some(msg_type) = MsgType::from_u8(value) {
                assert_eq!(msg_type.as_u8(), value);
                assert!((4..=MAX_MESSAGE_SIZE).contains(&msg_type.size()));
                known += 1;
            }
        }
        assert_eq!(known, 15);
        assert_eq!(MsgType::from_u8(0), None);
    }

    #[test]
    fn reports_are_the_five_answers_to_orders() {
        let reports = (0..=u8::MAX)
            .filter_map(MsgType::from_u8)
            .filter(|msg_type| msg_type.is_report())
            .count();
        assert_eq!(reports, 5);
        assert!(!MsgType::LoginAccepted.is_report());
        assert!(!MsgType::Heartbeat.is_report());
    }

    #[test]
    fn enums_refuse_the_first_value_past_their_end() {
        assert_eq!(Side::from_u8(2), None);
        assert_eq!(OrderType::from_u8(2), None);
        assert_eq!(TimeInForce::from_u8(3), None);
        assert_eq!(RejectCode::from_u8(10), None);
        assert_eq!(CancelReason::from_u8(3), None);
        assert_eq!(Liquidity::from_u8(2), None);
        assert_eq!(LoginRejectReason::from_u8(5), None);
        assert_eq!(RejectCode::Busy.name(), "Busy");
    }
}
