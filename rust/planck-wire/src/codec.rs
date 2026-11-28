//! How a struct becomes bytes. Private except for [`Message`].
//!
//! Each message is declared once, with the `message!` macro, as a table
//! of offsets and fields. The offsets are the ones the `static_assert`s
//! in the C++ headers pin down. Bytes the table does not name are
//! reserved: written as zero and required to be zero.

use crate::HEADER_SIZE;
use crate::error::WireError;
use crate::types::MsgType;

/// A message of the protocol.
pub trait Message: Sized {
    /// The type code in the header.
    const TYPE: MsgType;
    /// Size on the wire in bytes, header included.
    const SIZE: usize;

    /// Write the message to the front of `out` and return its size.
    ///
    /// # Errors
    ///
    /// [`WireError::BufferTooSmall`] if `out` is shorter than the message.
    fn encode(&self, out: &mut [u8]) -> Result<usize, WireError>;

    /// Read the message at the front of `bytes`. Anything after it is
    /// left alone.
    ///
    /// # Errors
    ///
    /// Any [`WireError`] except `BufferTooSmall` and `BadPacket`.
    fn decode(bytes: &[u8]) -> Result<Self, WireError>;
}

/// One fixed-size little-endian field.
pub(crate) trait Field: Sized + Copy {
    const SIZE: usize;
    /// `out` is exactly `SIZE` bytes.
    fn put(self, out: &mut [u8]);
    /// `bytes` is exactly `SIZE` bytes. `None` if they hold no valid value.
    fn get(bytes: &[u8]) -> Option<Self>;
}

macro_rules! int_field {
    ($($ty:ty),+) => {$(
        impl Field for $ty {
            const SIZE: usize = size_of::<$ty>();

            fn put(self, out: &mut [u8]) {
                out.copy_from_slice(&self.to_le_bytes());
            }

            fn get(bytes: &[u8]) -> Option<Self> {
                Some(<$ty>::from_le_bytes(bytes.try_into().ok()?))
            }
        }
    )+};
}

int_field!(u8, u16, u32, u64, i32);

/// The four header bytes of a message of this type and size.
pub(crate) fn header_bytes(msg_type: MsgType, size: u16) -> [u8; HEADER_SIZE] {
    let length = size.to_le_bytes();
    [length[0], length[1], msg_type.as_u8(), 0]
}

/// Check that `bytes` starts with a whole message of this type, and
/// return exactly that message's bytes.
pub(crate) fn checked(bytes: &[u8], msg_type: MsgType) -> Result<&[u8], WireError> {
    let size = msg_type.size();
    let Some(header) = bytes.get(..HEADER_SIZE) else {
        return Err(WireError::Truncated {
            needed: size,
            got: bytes.len(),
        });
    };
    let Some(found) = MsgType::from_u8(header[2]) else {
        return Err(WireError::BadHeader);
    };
    let length = usize::from(u16::from_le_bytes([header[0], header[1]]));
    if length != found.size() || header[3] != 0 {
        return Err(WireError::BadHeader);
    }
    if found != msg_type {
        return Err(WireError::WrongType {
            expected: msg_type,
            got: found,
        });
    }
    bytes.get(..size).ok_or(WireError::Truncated {
        needed: size,
        got: bytes.len(),
    })
}

/// Declare a message: its struct, `to_bytes`, and [`Message`].
///
/// ```text
/// message! {
///     /// Doc comment.
///     Name: 16 {
///         8 => order_id: u64,
///     }
/// }
/// ```
///
/// `Name` must be a variant of [`MsgType`]. The number after it is the
/// size on the wire and each number before `=>` is a field's offset.
macro_rules! message {
    (
        $(#[$meta:meta])*
        $name:ident: $size:literal {
            $(
                $(#[$field_meta:meta])*
                $offset:literal => $field:ident: $ty:ty
            ),* $(,)?
        }
    ) => {
        $(#[$meta])*
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub struct $name {
            $(
                $(#[$field_meta])*
                pub $field: $ty,
            )*
        }

        impl $name {
            /// The message as it goes on the wire.
            #[must_use]
            pub fn to_bytes(&self) -> [u8; $size] {
                let mut out = [0u8; $size];
                out[..$crate::HEADER_SIZE].copy_from_slice(&$crate::codec::header_bytes(
                    $crate::types::MsgType::$name,
                    $size,
                ));
                $(
                    $crate::codec::Field::put(
                        self.$field,
                        &mut out[$offset..$offset + <$ty as $crate::codec::Field>::SIZE],
                    );
                )*
                out
            }
        }

        impl $crate::codec::Message for $name {
            const TYPE: $crate::types::MsgType = $crate::types::MsgType::$name;
            const SIZE: usize = $size;

            fn encode(&self, out: &mut [u8]) -> Result<usize, $crate::error::WireError> {
                let got = out.len();
                let Some(front) = out.get_mut(..$size) else {
                    return Err($crate::error::WireError::BufferTooSmall { needed: $size, got });
                };
                front.copy_from_slice(&self.to_bytes());
                Ok($size)
            }

            fn decode(bytes: &[u8]) -> Result<Self, $crate::error::WireError> {
                let bytes = $crate::codec::checked(bytes, $crate::types::MsgType::$name)?;
                let message = Self {
                    $(
                        $field: <$ty as $crate::codec::Field>::get(
                            &bytes[$offset..$offset + <$ty as $crate::codec::Field>::SIZE],
                        )
                        .ok_or($crate::error::WireError::BadField {
                            message: $crate::types::MsgType::$name,
                            field: stringify!($field),
                        })?,
                    )*
                };
                // Every named field survives the trip back to bytes, so a
                // difference can only be in a byte the table does not name.
                if message.to_bytes().as_slice() != bytes {
                    return Err($crate::error::WireError::ReservedNotZero {
                        message: $crate::types::MsgType::$name,
                    });
                }
                Ok(message)
            }
        }
    };
}

pub(crate) use message;
