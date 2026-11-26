#include "planck/wire/types.hpp"

#include <array>
#include <utility>

namespace planck::wire {

namespace {

constexpr std::array<std::string_view, kRejectCodeCount> kRejectNames{
    "None",        "InvalidQuantity", "PriceOutOfBand", "NotFillable", "BookFull",
    "UnknownOrder", "UnknownSymbol",  "Malformed",      "NotLoggedIn", "Busy",
};

static_assert(std::to_underlying(RejectCode::Busy) + 1 == kRejectCodeCount);
static_assert(kRejectNames[std::to_underlying(RejectCode::UnknownSymbol)] == "UnknownSymbol");
static_assert(kRejectNames[std::to_underlying(RejectCode::Busy)] == "Busy");

}  // namespace

std::string_view to_string(MsgType type) noexcept {
    switch (type) {
        case MsgType::None:
            return "None";
        case MsgType::LoginRequest:
            return "LoginRequest";
        case MsgType::LoginAccepted:
            return "LoginAccepted";
        case MsgType::LoginRejected:
            return "LoginRejected";
        case MsgType::Heartbeat:
            return "Heartbeat";
        case MsgType::Logout:
            return "Logout";
        case MsgType::NewOrder:
            return "NewOrder";
        case MsgType::CancelOrder:
            return "CancelOrder";
        case MsgType::OrderAccepted:
            return "OrderAccepted";
        case MsgType::OrderRejected:
            return "OrderRejected";
        case MsgType::Executed:
            return "Executed";
        case MsgType::Canceled:
            return "Canceled";
        case MsgType::CancelRejected:
            return "CancelRejected";
        case MsgType::MdAddOrder:
            return "MdAddOrder";
        case MsgType::MdExecuted:
            return "MdExecuted";
        case MsgType::MdDeleted:
            return "MdDeleted";
    }
    return "Unknown";
}

std::string_view to_string(RejectCode code) noexcept {
    const std::size_t index = std::to_underlying(code);
    if (index >= kRejectNames.size()) {
        return "Unknown";
    }
    return kRejectNames[index];
}

}  // namespace planck::wire
