//! The wire format, checked against bytes the C++ code produced.
//!
//! The files under `tests/fixtures/wire/v3/` at the top of the repository
//! are written by `tests/unit/wire_fixture_test.cpp`, which also checks
//! on every run that the C++ structs still produce them. The values here
//! repeat the ones in that file. Each test proves both directions: the
//! file decodes to the value, and the value encodes to the file.

use std::fmt::Debug;
use std::path::PathBuf;

use planck_wire::{
    CancelOrder, CancelReason, CancelRejected, Canceled, Executed, Heartbeat, Liquidity,
    LoginAccepted, LoginRejectReason, LoginRejected, LoginRequest, Logout, MdAddOrder, MdDeleted,
    MdExecuted, MdMessage, Message, NewOrder, OrderAccepted, OrderRejected, OrderType, Packet,
    PacketHeader, RejectCode, Side, TimeInForce, VenueMessage, next_frame,
};

const ORDER_ID: u64 = 0x0001_0000_0000_0007;
const RESTING_ID: u64 = 0x0001_0000_0000_0008;
const T0: u64 = 1_700_000_000_000_000_000;

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/wire/v3")
}

fn fixture(name: &str) -> Vec<u8> {
    let path = fixture_dir().join(format!("{name}.bin"));
    std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
}

/// Every file this suite reads, in name order.
const KNOWN: [&str; 18] = [
    "cancel_order",
    "cancel_rejected",
    "canceled",
    "executed",
    "heartbeat",
    "login_accepted",
    "login_rejected",
    "login_request",
    "logout",
    "md_add_order",
    "md_deleted",
    "md_executed",
    "md_heartbeat",
    "md_packet",
    "new_order",
    "order_accepted",
    "order_entry_stream",
    "order_rejected",
];

// A message added on the C++ side arrives here as a new file. This fails
// until the file has a test below and a line in KNOWN, so the Rust side
// cannot be forgotten.
#[test]
fn every_fixture_file_has_a_test_here() {
    let mut found: Vec<String> = std::fs::read_dir(fixture_dir())
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "bin"))
        .map(|path| path.file_stem().unwrap().to_string_lossy().into_owned())
        .collect();
    found.sort();
    assert_eq!(found, KNOWN);
}

fn check<M: Message + PartialEq + Debug>(name: &str, expected: &M) {
    let bytes = fixture(name);
    assert_eq!(bytes.len(), M::SIZE, "{name}: size");
    assert_eq!(&M::decode(&bytes).unwrap(), expected, "{name}: decode");
    let mut out = vec![0xFFu8; M::SIZE];
    assert_eq!(expected.encode(&mut out), Ok(M::SIZE));
    assert_eq!(out, bytes, "{name}: encode");
}

// ---- the values, as in wire_fixture_test.cpp ----

fn login_accepted() -> LoginAccepted {
    LoginAccepted {
        session_id: 0x0102,
        idle_timeout_ms: 5000,
        epoch: 0x2122_2324_2526_2728,
        next_report: 7,
        live_from: 9,
        last_client_order_id: 41,
    }
}

fn order_accepted() -> OrderAccepted {
    OrderAccepted {
        client_order_id: 42,
        order_id: ORDER_ID,
        timestamp: T0 + 1,
    }
}

fn executed() -> Executed {
    Executed {
        liquidity: Liquidity::Taker,
        order_id: ORDER_ID,
        match_id: 99,
        price: -125,
        qty: 300,
        leaves: 700,
        timestamp: T0 + 2,
    }
}

fn canceled() -> Canceled {
    Canceled {
        reason: CancelReason::Disconnected,
        order_id: ORDER_ID,
        qty: 700,
        timestamp: T0 + 3,
    }
}

fn md_add_order() -> MdAddOrder {
    MdAddOrder {
        symbol_id: 0x1234,
        side: Side::Sell,
        order_id: RESTING_ID,
        price: 10_050,
        qty: 1000,
        timestamp: T0 + 4,
    }
}

fn md_executed() -> MdExecuted {
    MdExecuted {
        symbol_id: 0x1234,
        order_id: RESTING_ID,
        match_id: 100,
        qty: 250,
        price: 10_050,
        timestamp: T0 + 5,
    }
}

fn md_deleted() -> MdDeleted {
    MdDeleted {
        symbol_id: 0x1234,
        order_id: RESTING_ID,
        timestamp: T0 + 6,
    }
}

// ---- session ----

#[test]
fn login_request() {
    check(
        "login_request",
        &LoginRequest::resuming(
            0x0102_0304,
            0x1112_1314_1516_1718,
            0x2122_2324_2526_2728,
            0x3132_3334_3536_3738,
        ),
    );
}

#[test]
fn login_accepted_and_rejected() {
    check("login_accepted", &login_accepted());
    check(
        "login_rejected",
        &LoginRejected {
            reason: LoginRejectReason::BadSequence,
        },
    );
}

#[test]
fn heartbeat_and_logout() {
    check("heartbeat", &Heartbeat {});
    check("logout", &Logout {});
}

// ---- order entry ----

#[test]
fn new_order_and_cancel_order() {
    check(
        "new_order",
        &NewOrder {
            symbol_id: 0x1234,
            side: Side::Sell,
            order_type: OrderType::Limit,
            client_order_id: 0x1122_3344_5566_7788,
            price: -2,
            qty: 0x0A0B_0C0D,
            tif: TimeInForce::FillOrKill,
        },
    );
    check("cancel_order", &CancelOrder { order_id: ORDER_ID });
}

#[test]
fn reports() {
    check("order_accepted", &order_accepted());
    check(
        "order_rejected",
        &OrderRejected {
            reason: RejectCode::PriceOutOfBand,
            client_order_id: 43,
        },
    );
    check("executed", &executed());
    check("canceled", &canceled());
    check(
        "cancel_rejected",
        &CancelRejected {
            reason: RejectCode::UnknownOrder,
            order_id: 0x0001_0000_0000_0009,
        },
    );
}

#[test]
fn order_entry_stream_cuts_into_five_messages() {
    let stream = fixture("order_entry_stream");
    let mut rest = stream.as_slice();
    let mut seen = Vec::new();
    while let Some(frame) = next_frame(rest).unwrap() {
        seen.push(VenueMessage::decode(frame).unwrap());
        rest = &rest[frame.bytes.len()..];
    }
    assert!(rest.is_empty());
    assert_eq!(
        seen,
        vec![
            VenueMessage::LoginAccepted(login_accepted()),
            VenueMessage::OrderAccepted(order_accepted()),
            VenueMessage::Executed(executed()),
            VenueMessage::Heartbeat,
            VenueMessage::Canceled(canceled()),
        ]
    );
    // Three of the five are numbered: the client's count goes from 7 to 10.
    let reports = seen.iter().filter(|message| message.is_report()).count();
    assert_eq!(reports, 3);
}

#[test]
fn order_entry_stream_survives_any_split() {
    // A read may end anywhere. Feeding the stream in two pieces, cut at
    // every possible place, must give the same five messages.
    let stream = fixture("order_entry_stream");
    for cut in 0..=stream.len() {
        let mut buffer = Vec::new();
        let mut count = 0;
        for piece in [&stream[..cut], &stream[cut..]] {
            buffer.extend_from_slice(piece);
            while let Some(frame) = next_frame(&buffer).unwrap() {
                VenueMessage::decode(frame).unwrap();
                let used = frame.bytes.len();
                buffer.drain(..used);
                count += 1;
            }
        }
        assert_eq!(count, 5, "cut at {cut}");
        assert!(buffer.is_empty(), "cut at {cut}");
    }
}

// ---- market data ----

#[test]
fn market_data_messages() {
    check("md_add_order", &md_add_order());
    check("md_executed", &md_executed());
    check("md_deleted", &md_deleted());
}

#[test]
fn market_data_packet() {
    let bytes = fixture("md_packet");
    let packet = Packet::parse(&bytes).unwrap();
    let header = PacketHeader {
        sequence: 1000,
        count: 3,
        stream_id: 2,
    };
    assert_eq!(packet.header, header);
    assert_eq!(header.to_bytes(), bytes[..PacketHeader::SIZE]);
    let seen: Vec<_> = packet.messages().collect();
    assert_eq!(
        seen,
        vec![
            (1000, MdMessage::AddOrder(md_add_order())),
            (1001, MdMessage::Executed(md_executed())),
            (1002, MdMessage::Deleted(md_deleted())),
        ]
    );
    assert_eq!(packet.header.next_sequence(), 1003);
}

#[test]
fn market_data_heartbeat() {
    let bytes = fixture("md_heartbeat");
    let packet = Packet::parse(&bytes).unwrap();
    assert!(packet.header.is_heartbeat());
    assert_eq!(packet.header.sequence, 1003);
    assert_eq!(packet.header.stream_id, 2);
    assert_eq!(packet.messages().count(), 0);
    assert_eq!(packet.header.to_bytes().as_slice(), bytes);
}
