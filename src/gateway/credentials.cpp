#include "planck/gateway/credentials.hpp"

#include <charconv>
#include <cstdio>
#include <memory>

#include "planck/core/contract.hpp"

namespace planck::gateway {

namespace {

using core::ok;
using core::Status;

constexpr bool blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

// Take the next run of non-blank characters off the front of `line`.
std::string_view next_word(std::string_view& line) noexcept {
    std::size_t start = 0;
    while (start < line.size() && blank(line[start])) {
        ++start;
    }
    std::size_t end = start;
    while (end < line.size() && !blank(line[end])) {
        ++end;
    }
    const std::string_view word = line.substr(start, end - start);
    line.remove_prefix(end);
    return word;
}

// A whole number, decimal or 0x hexadecimal, and nothing else.
bool number(std::string_view word, std::uint64_t& out) noexcept {
    int base = 10;
    if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
        base = 16;
        word.remove_prefix(2);
    }
    if (word.empty()) {
        return false;
    }
    const char* const end = word.data() + word.size();
    const std::from_chars_result result = std::from_chars(word.data(), end, out, base);
    return result.ec == std::errc{} && result.ptr == end;
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};

}  // namespace

bool Credentials::listed(std::uint32_t client_id) const noexcept {
    PLANCK_EXPECT(count_ <= entries_.size());
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].client_id == client_id) {
            return true;
        }
    }
    return false;
}

Status Credentials::add(std::uint32_t client_id, std::uint64_t token) noexcept {
    if (client_id == 0 || client_id > kMaxClientId) {
        return Status::OutOfRange;
    }
    if (listed(client_id)) {
        return Status::Duplicate;
    }
    if (count_ == entries_.size()) {
        return Status::Capacity;
    }
    entries_[count_] = Entry{.token = token, .client_id = client_id};
    ++count_;
    return Status::Ok;
}

Status Credentials::parse(std::string_view text, std::uint32_t& bad_line) noexcept {
    const std::size_t before = count_;
    bad_line = 0;
    std::uint32_t line_number = 0;
    Status status = Status::Ok;

    // Each pass removes one line, so the length of the text bounds the loop.
    while (!text.empty() && ok(status)) {
        ++line_number;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix((newline == std::string_view::npos) ? text.size() : newline + 1);

        if (line.size() > kMaxLineBytes) {
            status = Status::Protocol;
            break;
        }
        line = line.substr(0, line.find('#'));
        const std::string_view id_word = next_word(line);
        if (id_word.empty()) {
            continue;  // blank, or only a comment
        }
        const std::string_view token_word = next_word(line);
        const std::string_view extra = next_word(line);
        std::uint64_t id = 0;
        std::uint64_t token = 0;
        if (!extra.empty() || !number(id_word, id) || !number(token_word, token)) {
            status = Status::Protocol;
        } else if (id > kMaxClientId) {
            status = Status::OutOfRange;
        } else {
            status = add(static_cast<std::uint32_t>(id), token);
        }
    }

    if (!ok(status)) {
        count_ = before;  // all or nothing
        bad_line = line_number;
    }
    return status;
}

Status Credentials::load(const char* path, std::uint32_t& bad_line) noexcept {
    bad_line = 0;
    if (path == nullptr) {
        return Status::InvalidArgument;
    }
    const std::unique_ptr<std::FILE, FileCloser> file(std::fopen(path, "rb"));
    if (file == nullptr) {
        return Status::Io;
    }
    // One byte more than the limit, so a file that is too large is noticed.
    const auto buffer = std::make_unique_for_overwrite<char[]>(kMaxFileBytes + 1);
    const std::size_t length = std::fread(buffer.get(), 1, kMaxFileBytes + 1, file.get());
    if (std::ferror(file.get()) != 0) {
        return Status::Io;
    }
    if (length > kMaxFileBytes) {
        return Status::Capacity;
    }
    return parse({buffer.get(), length}, bad_line);
}

bool Credentials::permits(std::uint32_t client_id, std::uint64_t token) const noexcept {
    PLANCK_EXPECT(count_ <= entries_.size());
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].client_id == client_id) {
            return entries_[i].token == token;
        }
    }
    return false;
}

}  // namespace planck::gateway
