#ifndef PLANCK_TESTS_SUPPORT_FIXTURES_HPP
#define PLANCK_TESTS_SUPPORT_FIXTURES_HPP

#include <cstddef>
#include <cstdlib>
#include <vector>

#include "planck/book/types.hpp"

namespace planck::test {

// A TradeSink that keeps every fill so a test can look at them afterwards.
struct TradeLog {
    std::vector<book::Trade> trades;
    void on_trade(const book::Trade& trade) noexcept { trades.push_back(trade); }
};

// Size of a randomized run. Set PLANCK_TEST_SCALE=20 in the environment to
// run twenty times as many operations as the default.
inline std::size_t scaled(std::size_t base) {
    const char* const text = std::getenv("PLANCK_TEST_SCALE");
    if (text == nullptr) {
        return base;
    }
    const long scale = std::strtol(text, nullptr, 10);
    return (scale >= 1 && scale <= 100000) ? base * static_cast<std::size_t>(scale) : base;
}

}  // namespace planck::test

#endif  // PLANCK_TESTS_SUPPORT_FIXTURES_HPP
