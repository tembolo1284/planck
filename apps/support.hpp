#ifndef PLANCK_APPS_SUPPORT_HPP
#define PLANCK_APPS_SUPPORT_HPP

#include <array>
#include <atomic>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <system_error>

#include "planck/net/address.hpp"

// Shared by the planck command line programs: argument parsing, a few
// defaults, and small formatting helpers. None of this is on a hot path.
namespace planck::apps {

// Where the programs find each other when nothing is said on the command line.
inline constexpr std::uint16_t kDefaultOrderPort = 9000;
inline constexpr std::uint16_t kDefaultRetransmitPort = 30000;
inline constexpr std::uint16_t kDefaultFeedPort = 30001;
// Administratively scoped multicast: stays inside one site.
inline constexpr std::uint32_t kDefaultFeedGroup = net::ipv4(239, 255, 42, 1);

// Parse a whole string as a number in [low, high]. out is written only on
// success.
template <std::integral T>
[[nodiscard]] bool parse_number(std::string_view text, T& out, T low, T high) noexcept {
    T value{};
    const char* const end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < low || value > high) {
        return false;
    }
    out = value;
    return true;
}

// Parse "a.b.c.d:port". out is written only on success.
[[nodiscard]] bool parse_endpoint(std::string_view text, net::Endpoint& out) noexcept;

// Split a line into words separated by spaces or tabs. Returns how many were
// found, or out.size() + 1 if there were too many to fit.
[[nodiscard]] std::size_t split_words(std::string_view line,
                                      std::span<std::string_view> out) noexcept;

// Command line of the form
//     program --name value --other=value --flag
// Each lookup marks what it used, so anything left over at the end is a
// mistake the program can report. Lookups print their own error to stderr
// and return false if a value is present but unusable. An option that is
// simply absent is not an error: the output keeps its default.
class Args {
public:
    static constexpr std::size_t kMaxArgs = 64;

    Args(int argc, const char* const* argv) noexcept;

    // True if --name was given.
    [[nodiscard]] bool flag(std::string_view name) noexcept;

    [[nodiscard]] bool text(std::string_view name, std::string_view& out) noexcept;
    [[nodiscard]] bool endpoint(std::string_view name, net::Endpoint& out) noexcept;

    template <std::integral T>
    [[nodiscard]] bool number(std::string_view name, T& out, T low, T high) noexcept {
        std::string_view value;
        if (!find(name, value)) {
            return !bad_;
        }
        if (!parse_number(value, out, low, high)) {
            std::fprintf(stderr, "%.*s: expected a number from %lld to %llu, got '%.*s'\n",
                         static_cast<int>(name.size()), name.data(), static_cast<long long>(low),
                         static_cast<unsigned long long>(high), static_cast<int>(value.size()),
                         value.data());
            return false;
        }
        return true;
    }

    // Call after every lookup. Prints and returns false if an argument was
    // never asked for, which means it was misspelt or does not exist.
    [[nodiscard]] bool finish() const noexcept;

private:
    // Find --name and its value. False if absent, or if present without a
    // value, in which case bad_ is set and an error has been printed.
    [[nodiscard]] bool find(std::string_view name, std::string_view& value) noexcept;

    std::array<std::string_view, kMaxArgs> args_{};
    std::array<bool, kMaxArgs> used_{};
    std::size_t count_ = 0;
    bool bad_ = false;
};

// A short piece of formatted text, returned by value so it can be handed
// straight to printf.
struct ShortText {
    std::array<char, 24> chars{};
    [[nodiscard]] const char* c_str() const noexcept { return chars.data(); }
};

// Nanoseconds since the Unix epoch as "HH:MM:SS.nnnnnnnnn" (UTC).
[[nodiscard]] ShortText format_time(std::uint64_t epoch_ns) noexcept;

// "a.b.c.d:port".
[[nodiscard]] ShortText format_endpoint(net::Endpoint endpoint) noexcept;

// Arrange for SIGINT and SIGTERM to set a flag instead of killing the
// process, and return that flag. Safe to call more than once.
[[nodiscard]] const std::atomic<bool>& stop_on_signal() noexcept;

}  // namespace planck::apps

#endif  // PLANCK_APPS_SUPPORT_HPP
