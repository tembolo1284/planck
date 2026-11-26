#include "planck/book/types.hpp"

#include <array>
#include <utility>

namespace planck::book {

namespace {

constexpr std::array<std::string_view, kRejectReasonCount> kRejectNames{
    "None", "InvalidQuantity", "PriceOutOfBand", "NotFillable", "BookFull", "UnknownOrder",
};

static_assert(std::to_underlying(RejectReason::UnknownOrder) + 1 == kRejectReasonCount);
static_assert(kRejectNames[std::to_underlying(RejectReason::None)] == "None");
static_assert(kRejectNames[std::to_underlying(RejectReason::UnknownOrder)] == "UnknownOrder");

}  // namespace

std::string_view to_string(RejectReason reason) noexcept {
    const std::size_t index = std::to_underlying(reason);
    if (index >= kRejectNames.size()) {
        return "Unknown";
    }
    return kRejectNames[index];
}

}  // namespace planck::book
