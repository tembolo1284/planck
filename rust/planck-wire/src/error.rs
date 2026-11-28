//! What can be wrong with bytes that claim to be planck messages.

use core::fmt;

use crate::types::MsgType;

/// Why bytes could not be read as a message, or a message not written.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WireError {
    /// Fewer bytes than the message needs. On a stream this means read
    /// more. In a datagram it means the datagram is damaged.
    Truncated { needed: usize, got: usize },
    /// The output buffer cannot hold the message.
    BufferTooSmall { needed: usize, got: usize },
    /// The header names no message of this protocol, gives a length that
    /// is not that message's size, or has its reserved byte set. On a
    /// stream there is no way back in step: drop the connection.
    BadHeader,
    /// A well-formed message, but not the one asked for.
    WrongType { expected: MsgType, got: MsgType },
    /// A field holds a value outside its enum.
    BadField {
        message: MsgType,
        field: &'static str,
    },
    /// A reserved byte is not zero.
    ReservedNotZero { message: MsgType },
    /// A market data datagram whose header and contents disagree.
    BadPacket(&'static str),
}

impl fmt::Display for WireError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated { needed, got } => {
                write!(f, "message needs {needed} bytes, only {got} present")
            }
            Self::BufferTooSmall { needed, got } => {
                write!(f, "message needs {needed} bytes, buffer holds {got}")
            }
            Self::BadHeader => f.write_str("header is not that of any planck message"),
            Self::WrongType { expected, got } => {
                write!(f, "expected {}, found {}", expected.name(), got.name())
            }
            Self::BadField { message, field } => {
                write!(f, "{}: field `{field}` is out of range", message.name())
            }
            Self::ReservedNotZero { message } => {
                write!(f, "{}: a reserved byte is not zero", message.name())
            }
            Self::BadPacket(why) => write!(f, "bad market data packet: {why}"),
        }
    }
}

impl std::error::Error for WireError {}
