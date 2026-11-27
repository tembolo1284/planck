#ifndef PLANCK_GATEWAY_CREDENTIALS_HPP
#define PLANCK_GATEWAY_CREDENTIALS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "planck/core/status.hpp"

namespace planck::gateway {

// Who may log in, and the token each must present.
//
// A client id is also that client's session id, so it must fit in 16 bits
// and may not be zero. Built once at startup and read-only afterwards. The
// gateway looks a client up only at login, so a plain table and a scan are
// enough.
//
// The token is a shared secret sent in the clear. It keeps one client from
// logging in as another. It does not protect anyone on a network where
// strangers can read or alter the traffic: for that the link itself must be
// private (a private network, or a tunnel such as WireGuard).
class Credentials {
public:
    static constexpr std::size_t kMaxClients = 4096;
    static constexpr std::uint32_t kMaxClientId = 0xFFFF;
    // The longest line parse() accepts, comments included.
    static constexpr std::size_t kMaxLineBytes = 256;
    // The largest file load() accepts.
    static constexpr std::size_t kMaxFileBytes = kMaxClients * 64;

    // OutOfRange if the id is zero or above kMaxClientId. Duplicate if it is
    // already listed. Capacity if the table is full.
    [[nodiscard]] core::Status add(std::uint32_t client_id, std::uint64_t token) noexcept;

    // Add every client in a text. One client per line:
    //
    //     # comment
    //     CLIENT_ID TOKEN
    //
    // Both are whole numbers, decimal or hexadecimal with a leading 0x.
    // Blank lines and anything after a # are ignored. On any error nothing
    // is added, `bad_line` is the line at fault counting from 1, and the
    // status says why: Protocol for a line that cannot be read, otherwise
    // whatever add() said.
    [[nodiscard]] core::Status parse(std::string_view text, std::uint32_t& bad_line) noexcept;

    // parse() the contents of a file. Io if it cannot be read, Capacity if
    // it is larger than kMaxFileBytes. `bad_line` is zero for those.
    [[nodiscard]] core::Status load(const char* path, std::uint32_t& bad_line) noexcept;

    // True if this client is listed with exactly this token.
    [[nodiscard]] bool permits(std::uint32_t client_id, std::uint64_t token) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Entry {
        std::uint64_t token = 0;
        std::uint32_t client_id = 0;
    };

    [[nodiscard]] bool listed(std::uint32_t client_id) const noexcept;

    std::size_t count_ = 0;
    std::array<Entry, kMaxClients> entries_{};
};

}  // namespace planck::gateway

#endif  // PLANCK_GATEWAY_CREDENTIALS_HPP
