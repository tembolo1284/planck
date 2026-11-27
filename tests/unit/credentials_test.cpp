#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "planck/gateway/credentials.hpp"
#include "support/temp_dir.hpp"

namespace planck::gateway {
namespace {

using core::Status;

// The table is 64 KiB, so it lives on the heap in every test.
std::unique_ptr<Credentials> table() { return std::make_unique<Credentials>(); }

TEST(CredentialsTest, PermitsExactlyTheListedPairs) {
    const auto clients = table();
    EXPECT_FALSE(clients->permits(7, 0)) << "an empty table lets nobody in";
    ASSERT_EQ(clients->add(7, 1234), Status::Ok);
    ASSERT_EQ(clients->add(8, 0), Status::Ok);
    EXPECT_EQ(clients->size(), 2U);

    EXPECT_TRUE(clients->permits(7, 1234));
    EXPECT_FALSE(clients->permits(7, 1235));
    EXPECT_FALSE(clients->permits(7, 0));
    EXPECT_FALSE(clients->permits(8, 1234)) << "one client's token opened another's door";
    EXPECT_TRUE(clients->permits(8, 0));
    EXPECT_FALSE(clients->permits(9, 1234));
    EXPECT_FALSE(clients->permits(0, 0));
    EXPECT_FALSE(clients->permits(65'536 + 7, 1234)) << "ids are not compared in 16 bits";
}

TEST(CredentialsTest, RefusesIdsThatCannotBeSessionsAndRepeats) {
    const auto clients = table();
    EXPECT_EQ(clients->add(0, 1), Status::OutOfRange);
    EXPECT_EQ(clients->add(65'536, 1), Status::OutOfRange);
    EXPECT_EQ(clients->add(65'535, 1), Status::Ok);
    EXPECT_EQ(clients->add(65'535, 2), Status::Duplicate);
    EXPECT_TRUE(clients->permits(65'535, 1)) << "a refused repeat replaced the token";
    EXPECT_EQ(clients->size(), 1U);
}

TEST(CredentialsTest, HoldsItsCapacityAndNoMore) {
    const auto clients = table();
    for (std::uint32_t id = 1; id <= Credentials::kMaxClients; ++id) {
        ASSERT_EQ(clients->add(id, id * 3ULL), Status::Ok) << id;
    }
    EXPECT_EQ(clients->add(60'000, 1), Status::Capacity);
    EXPECT_TRUE(clients->permits(1, 3));
    EXPECT_TRUE(clients->permits(Credentials::kMaxClients, Credentials::kMaxClients * 3ULL));
    EXPECT_FALSE(clients->permits(60'000, 1));
}

TEST(CredentialsTest, ParsesNumbersCommentsAndBlankLines) {
    const auto clients = table();
    std::uint32_t bad_line = 99;
    const Status status = clients->parse("# desk A\n"
                                         "\n"
                                         "1 1001\n"
                                         "  2\t0xFEED   # trailing comment\r\n"
                                         "0x10 18446744073709551615\n"
                                         "   # only a comment\n"
                                         "65535 0",  // no newline at the end
                                         bad_line);
    ASSERT_EQ(status, Status::Ok);
    EXPECT_EQ(bad_line, 0U);
    EXPECT_EQ(clients->size(), 4U);
    EXPECT_TRUE(clients->permits(1, 1001));
    EXPECT_TRUE(clients->permits(2, 0xFEED));
    EXPECT_TRUE(clients->permits(16, ~std::uint64_t{0}));
    EXPECT_TRUE(clients->permits(65'535, 0));
}

TEST(CredentialsTest, ABadLineIsNamedAndNothingIsAdded) {
    struct Case {
        const char* text;
        Status status;
        std::uint32_t line;
    };
    const Case cases[] = {
        {"1 10\n2\n", Status::Protocol, 2},                 // no token
        {"1 10\n2 20 30\n", Status::Protocol, 2},           // one word too many
        {"1 10\n\n# x\nabc 20\n", Status::Protocol, 4},     // not a number
        {"1 10\n2 20x\n", Status::Protocol, 2},             // junk after the number
        {"1 10\n2 -5\n", Status::Protocol, 2},              // no negative tokens
        {"1 10\n2 0x\n", Status::Protocol, 2},              // a prefix and no digits
        {"1 18446744073709551616\n", Status::Protocol, 1},  // one more than fits
        {"0 10\n", Status::OutOfRange, 1},
        {"1 10\n65536 20\n", Status::OutOfRange, 2},
        {"1 10\n2 20\n1 30\n", Status::Duplicate, 3},
    };
    for (const Case& c : cases) {
        const auto clients = table();
        ASSERT_EQ(clients->add(500, 5), Status::Ok);
        std::uint32_t bad_line = 0;
        EXPECT_EQ(clients->parse(c.text, bad_line), c.status) << c.text;
        EXPECT_EQ(bad_line, c.line) << c.text;
        EXPECT_EQ(clients->size(), 1U) << "part of a bad file was kept: " << c.text;
        EXPECT_FALSE(clients->permits(1, 10)) << c.text;
        EXPECT_TRUE(clients->permits(500, 5)) << "what was there before must survive";
    }

    const auto clients = table();
    std::uint32_t bad_line = 0;
    const std::string long_line = "1 10 #" + std::string(Credentials::kMaxLineBytes, 'x') + "\n";
    EXPECT_EQ(clients->parse(long_line, bad_line), Status::Protocol);
    EXPECT_EQ(bad_line, 1U);
}

TEST(CredentialsTest, LoadsAFileAndReportsWhatIsWrongWithOne) {
    const test::TempDir directory;
    ASSERT_TRUE(directory.ok());
    const auto write = [&](const std::string& name, const std::string& text) {
        const std::string path = directory.file(name);
        std::FILE* const file = std::fopen(path.c_str(), "wb");
        EXPECT_NE(file, nullptr);
        if (file != nullptr) {
            EXPECT_EQ(std::fwrite(text.data(), 1, text.size(), file), text.size());
            EXPECT_EQ(std::fclose(file), 0);
        }
        return path;
    };

    std::uint32_t bad_line = 99;
    auto clients = table();
    ASSERT_EQ(clients->load(write("good", "1 11\n2 22\n").c_str(), bad_line), Status::Ok);
    EXPECT_EQ(bad_line, 0U);
    EXPECT_TRUE(clients->permits(2, 22));

    clients = table();
    EXPECT_EQ(clients->load(write("bad", "1 11\n2\n").c_str(), bad_line), Status::Protocol);
    EXPECT_EQ(bad_line, 2U);
    EXPECT_EQ(clients->size(), 0U);

    bad_line = 99;
    EXPECT_EQ(clients->load(directory.file("missing").c_str(), bad_line), Status::Io);
    EXPECT_EQ(bad_line, 0U);
    EXPECT_EQ(clients->load(nullptr, bad_line), Status::InvalidArgument);

    const std::string huge(Credentials::kMaxFileBytes + 1, '\n');
    EXPECT_EQ(clients->load(write("huge", huge).c_str(), bad_line), Status::Capacity);
    const std::string largest(Credentials::kMaxFileBytes, '\n');
    EXPECT_EQ(clients->load(write("largest", largest).c_str(), bad_line), Status::Ok);
    EXPECT_EQ(clients->size(), 0U) << "a file of blank lines lists nobody";
}

}  // namespace
}  // namespace planck::gateway
