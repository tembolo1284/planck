#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "support.hpp"

namespace planck::apps {
namespace {

// Builds an argv the way main() receives it.
struct CommandLine {
    explicit CommandLine(std::vector<const char*> words) : argv(std::move(words)) {
        argv.insert(argv.begin(), "program");
    }
    [[nodiscard]] Args args() const { return Args(static_cast<int>(argv.size()), argv.data()); }
    std::vector<const char*> argv;
};

TEST(ParseNumber, AcceptsOnlyWholeNumbersInRange) {
    std::uint16_t small = 7;
    EXPECT_TRUE(parse_number<std::uint16_t>("0", small, 0, 100));
    EXPECT_EQ(small, 0);
    EXPECT_TRUE(parse_number<std::uint16_t>("100", small, 0, 100));
    EXPECT_EQ(small, 100);

    for (const std::string_view text : {"", " 5", "5 ", "5x", "x5", "101", "-1", "1.5", "0x10", "+5",
                                        "99999999999999999999"}) {
        small = 42;
        EXPECT_FALSE(parse_number<std::uint16_t>(text, small, 0, 100)) << "'" << text << "'";
        EXPECT_EQ(small, 42) << "output touched on failure for '" << text << "'";
    }

    std::int32_t price = 0;
    EXPECT_TRUE(parse_number<std::int32_t>("-250", price, INT32_MIN, INT32_MAX));
    EXPECT_EQ(price, -250);
    std::uint64_t big = 0;
    EXPECT_TRUE(parse_number<std::uint64_t>("18446744073709551615", big, 0, UINT64_MAX));
    EXPECT_EQ(big, UINT64_MAX);
    EXPECT_FALSE(parse_number<std::uint64_t>("18446744073709551616", big, 0, UINT64_MAX));
}

TEST(ParseEndpoint, NeedsAnAddressAndAPort) {
    net::Endpoint endpoint;
    ASSERT_TRUE(parse_endpoint("127.0.0.1:9000", endpoint));
    EXPECT_EQ(endpoint, (net::Endpoint{net::kLoopback, 9000}));
    ASSERT_TRUE(parse_endpoint("239.255.42.1:0", endpoint));
    EXPECT_EQ(endpoint, (net::Endpoint{net::ipv4(239, 255, 42, 1), 0}));

    for (const std::string_view text : {"", "127.0.0.1", "127.0.0.1:", ":9000", "9000",
                                        "127.0.0.1:65536", "127.0.0.1:-1", "localhost:9000",
                                        "127.0.0.1:9000:1", "127.0.0.1 :9000"}) {
        net::Endpoint untouched{1, 2};
        EXPECT_FALSE(parse_endpoint(text, untouched)) << "'" << text << "'";
        EXPECT_EQ(untouched, (net::Endpoint{1, 2}));
    }
}

TEST(SplitWords, SeparatesOnBlanks) {
    std::array<std::string_view, 4> words{};
    EXPECT_EQ(split_words("", words), 0U);
    EXPECT_EQ(split_words("   \t ", words), 0U);

    ASSERT_EQ(split_words("buy 1 10 1000", words), 4U);
    EXPECT_EQ(words[0], "buy");
    EXPECT_EQ(words[3], "1000");

    ASSERT_EQ(split_words("  cancel\t\tlast \r", words), 2U) << "tabs, runs of blanks, a DOS line end";
    EXPECT_EQ(words[0], "cancel");
    EXPECT_EQ(words[1], "last");

    EXPECT_EQ(split_words("a b c d e", words), 5U) << "one more than fits means too many";
}

TEST(ArgsTest, ReadsBothSpellingsAndKeepsDefaults) {
    const CommandLine line({"--shards", "4", "--listen=10.0.0.1:7000", "--quiet", "--name", "abc"});
    Args args = line.args();

    std::uint32_t shards = 1;
    std::uint32_t symbols = 8;
    net::Endpoint listen{};
    std::string_view name = "default";
    std::string_view absent = "default";
    EXPECT_TRUE(args.number<std::uint32_t>("--shards", shards, 1, 16));
    EXPECT_TRUE(args.number<std::uint32_t>("--symbols", symbols, 1, 100));
    EXPECT_TRUE(args.endpoint("--listen", listen));
    EXPECT_TRUE(args.text("--name", name));
    EXPECT_TRUE(args.text("--absent", absent));
    EXPECT_TRUE(args.flag("--quiet"));
    EXPECT_FALSE(args.flag("--verbose"));

    EXPECT_EQ(shards, 4U);
    EXPECT_EQ(symbols, 8U) << "an absent option keeps its default";
    EXPECT_EQ(listen, (net::Endpoint{net::ipv4(10, 0, 0, 1), 7000}));
    EXPECT_EQ(name, "abc");
    EXPECT_EQ(absent, "default");
    EXPECT_TRUE(args.finish());
}

TEST(ArgsTest, ReportsWhatItCannotUse) {
    {
        const CommandLine line({"--shards", "lots"});
        Args args = line.args();
        std::uint32_t shards = 1;
        EXPECT_FALSE(args.number<std::uint32_t>("--shards", shards, 1, 16));
        EXPECT_EQ(shards, 1U);
    }
    {
        const CommandLine line({"--shards", "17"});
        Args args = line.args();
        std::uint32_t shards = 1;
        EXPECT_FALSE(args.number<std::uint32_t>("--shards", shards, 1, 16)) << "out of range";
    }
    {
        const CommandLine line({"--listen", "nowhere"});
        Args args = line.args();
        net::Endpoint listen{};
        EXPECT_FALSE(args.endpoint("--listen", listen));
    }
    {
        const CommandLine line({"--shards"});
        Args args = line.args();
        std::uint32_t shards = 1;
        EXPECT_FALSE(args.number<std::uint32_t>("--shards", shards, 1, 16)) << "no value given";
        EXPECT_FALSE(args.finish());
    }
    {
        const CommandLine line({"--shardz", "4"});
        Args args = line.args();
        std::uint32_t shards = 1;
        EXPECT_TRUE(args.number<std::uint32_t>("--shards", shards, 1, 16));
        EXPECT_FALSE(args.finish()) << "a misspelt option must not pass silently";
    }
    {
        const CommandLine line({"--shards-extra=4"});
        Args args = line.args();
        std::uint32_t shards = 1;
        EXPECT_TRUE(args.number<std::uint32_t>("--shards", shards, 1, 16));
        EXPECT_EQ(shards, 1U) << "a longer option name must not match a shorter one";
        EXPECT_FALSE(args.finish());
    }
}

TEST(ArgsTest, RefusesMoreArgumentsThanItCanHold) {
    std::vector<const char*> many(Args::kMaxArgs + 1, "--flag");
    const CommandLine line(many);
    Args args = line.args();
    EXPECT_FALSE(args.finish());
}

TEST(FormatTime, ShowsTimeOfDayWithNanoseconds) {
    EXPECT_STREQ(format_time(0).c_str(), "00:00:00.000000000");
    // 1 day + 13:05:09 + 7 ns.
    const std::uint64_t seconds = 86'400 + (13 * 3600) + (5 * 60) + 9;
    EXPECT_STREQ(format_time((seconds * 1'000'000'000ULL) + 7).c_str(), "13:05:09.000000007");
    EXPECT_STREQ(format_time((86'399ULL * 1'000'000'000ULL) + 999'999'999).c_str(),
                 "23:59:59.999999999");
}

TEST(FormatEndpoint, IsDottedQuadAndPort) {
    EXPECT_STREQ(format_endpoint({net::kLoopback, 9000}).c_str(), "127.0.0.1:9000");
    EXPECT_STREQ(format_endpoint({net::ipv4(239, 255, 42, 1), 30001}).c_str(), "239.255.42.1:30001");
    // The longest possible result must fit.
    EXPECT_STREQ(format_endpoint({0xFFFFFFFFU, 65535}).c_str(), "255.255.255.255:65535");
}

}  // namespace
}  // namespace planck::apps
