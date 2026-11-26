#include "planck/core/status.hpp"

#include <array>
#include <utility>

namespace planck::core {

namespace {

constexpr std::array<std::string_view, kStatusCount> kStatusNames{
    "Ok",       "InvalidArgument", "OutOfRange", "Capacity", "NotFound",    "Duplicate",
    "WouldBlock", "Closed",        "Io",         "Protocol", "Unsupported", "Permission",
};

static_assert(std::to_underlying(Status::Permission) + 1 == kStatusCount);
static_assert(kStatusNames[std::to_underlying(Status::Ok)] == "Ok");
static_assert(kStatusNames[std::to_underlying(Status::Permission)] == "Permission");

}  // namespace

std::string_view to_string(Status status) noexcept {
    const std::size_t index = std::to_underlying(status);
    if (index >= kStatusNames.size()) {
        return "Unknown";
    }
    return kStatusNames[index];
}

}  // namespace planck::core
