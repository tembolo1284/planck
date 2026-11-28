//! Cutting a TCP byte stream into messages.

use crate::HEADER_SIZE;
use crate::error::WireError;
use crate::types::MsgType;

/// One whole message at the front of a buffer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Frame<'a> {
    pub msg_type: MsgType,
    /// Exactly the message's bytes, header included.
    pub bytes: &'a [u8],
}

/// Look for one whole message at the front of `buffer`.
///
/// `Ok(None)` means the message is not all here yet: read more and call
/// again. On `Ok(Some(frame))` consume `frame.bytes.len()` bytes and call
/// again. Safe on arbitrary bytes.
///
/// # Errors
///
/// [`WireError::BadHeader`] if the stream is corrupt. No message has a
/// variable length, so the length must agree exactly with the type, and
/// the first bad header is caught. Drop the connection.
pub fn next_frame(buffer: &[u8]) -> Result<Option<Frame<'_>>, WireError> {
    let Some(header) = buffer.get(..HEADER_SIZE) else {
        return Ok(None);
    };
    let Some(msg_type) = MsgType::from_u8(header[2]) else {
        return Err(WireError::BadHeader);
    };
    let length = usize::from(u16::from_le_bytes([header[0], header[1]]));
    if length != msg_type.size() || header[3] != 0 {
        return Err(WireError::BadHeader);
    }
    Ok(buffer.get(..length).map(|bytes| Frame { msg_type, bytes }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::order_entry::{CancelOrder, Heartbeat};

    #[test]
    fn a_partial_message_asks_for_more() {
        let bytes = CancelOrder { order_id: 7 }.to_bytes();
        for cut in 0..bytes.len() {
            assert_eq!(next_frame(&bytes[..cut]), Ok(None), "cut at {cut}");
        }
        let frame = next_frame(&bytes).unwrap().unwrap();
        assert_eq!(frame.msg_type, MsgType::CancelOrder);
        assert_eq!(frame.bytes, bytes);
    }

    #[test]
    fn a_frame_stops_at_the_end_of_its_message() {
        let mut stream = Heartbeat {}.to_bytes().to_vec();
        stream.extend_from_slice(&CancelOrder { order_id: 7 }.to_bytes());
        let first = next_frame(&stream).unwrap().unwrap();
        assert_eq!(first.msg_type, MsgType::Heartbeat);
        assert_eq!(first.bytes.len(), 4);
    }

    #[test]
    fn a_bad_header_is_an_error_at_once() {
        // Unknown type.
        assert_eq!(next_frame(&[4, 0, 0x7F, 0]), Err(WireError::BadHeader));
        // Type 0 is not a message.
        assert_eq!(next_frame(&[4, 0, 0, 0]), Err(WireError::BadHeader));
        // A heartbeat that claims five bytes.
        assert_eq!(next_frame(&[5, 0, 0x04, 0]), Err(WireError::BadHeader));
        // Reserved byte set.
        assert_eq!(next_frame(&[4, 0, 0x04, 1]), Err(WireError::BadHeader));
    }
}
