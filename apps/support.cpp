#include "support.hpp"

#include <csignal>

namespace planck::apps {

namespace {

std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the signal handler stores to it");

extern "C" void on_stop_signal(int /*signal*/) {
    g_stop.store(true, std::memory_order_relaxed);
}

void print_name(std::string_view name) noexcept {
    std::fprintf(stderr, "%.*s", static_cast<int>(name.size()), name.data());
}

}  // namespace

bool parse_endpoint(std::string_view text, net::Endpoint& out) noexcept {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    net::Endpoint endpoint;
    if (!core::ok(net::parse_ipv4(text.substr(0, colon), endpoint.address))) {
        return false;
    }
    if (!parse_number<std::uint16_t>(text.substr(colon + 1), endpoint.port, 0, 65535)) {
        return false;
    }
    out = endpoint;
    return true;
}

std::size_t split_words(std::string_view line, std::span<std::string_view> out) noexcept {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t count = 0;
    std::size_t pos = 0;
    // Each pass consumes at least one character, so the line length bounds it.
    while (pos < line.size()) {
        while (pos < line.size() && blank(line[pos])) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < line.size() && !blank(line[pos])) {
            ++pos;
        }
        if (pos == start) {
            break;
        }
        if (count == out.size()) {
            return out.size() + 1;
        }
        out[count] = line.substr(start, pos - start);
        ++count;
    }
    return count;
}

Args::Args(int argc, const char* const* argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (count_ == kMaxArgs) {
            std::fprintf(stderr, "too many arguments (the limit is %zu)\n", kMaxArgs);
            bad_ = true;
            break;
        }
        args_[count_] = argv[i];
        ++count_;
    }
}

bool Args::flag(std::string_view name) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (!used_[i] && args_[i] == name) {
            used_[i] = true;
            return true;
        }
    }
    return false;
}

bool Args::find(std::string_view name, std::string_view& value) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (used_[i]) {
            continue;
        }
        const std::string_view arg = args_[i];
        // --name=value
        if (arg.size() > name.size() && arg.starts_with(name) && arg[name.size()] == '=') {
            used_[i] = true;
            value = arg.substr(name.size() + 1);
            return true;
        }
        // --name value
        if (arg == name) {
            used_[i] = true;
            if ((i + 1) >= count_ || used_[i + 1]) {
                print_name(name);
                std::fprintf(stderr, ": needs a value\n");
                bad_ = true;
                return false;
            }
            used_[i + 1] = true;
            value = args_[i + 1];
            return true;
        }
    }
    return false;
}

bool Args::text(std::string_view name, std::string_view& out) noexcept {
    std::string_view value;
    if (find(name, value)) {
        out = value;
    }
    return !bad_;
}

bool Args::endpoint(std::string_view name, net::Endpoint& out) noexcept {
    std::string_view value;
    if (!find(name, value)) {
        return !bad_;
    }
    if (!parse_endpoint(value, out)) {
        print_name(name);
        std::fprintf(stderr, ": expected ADDRESS:PORT such as 127.0.0.1:9000, got '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return false;
    }
    return true;
}

bool Args::finish() const noexcept {
    if (bad_) {
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (!used_[i]) {
            std::fprintf(stderr, "unknown argument '%.*s' (try --help)\n",
                         static_cast<int>(args_[i].size()), args_[i].data());
            return false;
        }
    }
    return true;
}

ShortText format_time(std::uint64_t epoch_ns) noexcept {
    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
    const std::uint64_t seconds = epoch_ns / kNanosPerSecond;
    const std::uint64_t in_day = seconds % 86'400;
    ShortText text;
    static_cast<void>(std::snprintf(
        text.chars.data(), text.chars.size(), "%02u:%02u:%02u.%09u",
        static_cast<unsigned>(in_day / 3600), static_cast<unsigned>((in_day / 60) % 60),
        static_cast<unsigned>(in_day % 60), static_cast<unsigned>(epoch_ns % kNanosPerSecond)));
    return text;
}

ShortText format_endpoint(net::Endpoint endpoint) noexcept {
    ShortText text;
    static_cast<void>(std::snprintf(text.chars.data(), text.chars.size(), "%u.%u.%u.%u:%u",
                                    endpoint.address >> 24, (endpoint.address >> 16) & 0xFFU,
                                    (endpoint.address >> 8) & 0xFFU, endpoint.address & 0xFFU,
                                    endpoint.port));
    return text;
}

const std::atomic<bool>& stop_on_signal() noexcept {
    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    static_cast<void>(sigemptyset(&action.sa_mask));
    // No SA_RESTART: a blocked poll() or sleep should return so the main
    // loop sees the flag.
    static_cast<void>(sigaction(SIGINT, &action, nullptr));
    static_cast<void>(sigaction(SIGTERM, &action, nullptr));
    // A peer that goes away must show up as an error from send(), never as
    // a signal that kills the process.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    return g_stop;
}

}  // namespace planck::apps
