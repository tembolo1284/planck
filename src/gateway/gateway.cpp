#include "planck/gateway/gateway.hpp"

#include <cstring>
#include <utility>

#include <sched.h>

#include "planck/book/order_store.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"
#include "planck/sys/clock.hpp"
#include "planck/sys/cpu.hpp"

namespace planck::gateway {

namespace {

using core::ok;
using core::Status;

Status validate(const GatewayConfig& config) noexcept {
    if (config.shards.empty() || config.shards.size() > core::kMaxShards) {
        return Status::InvalidArgument;
    }
    if (config.max_sessions == 0 || config.max_sessions > core::kMaxSessions) {
        return Status::InvalidArgument;
    }
    // A connection must at least be able to hold one of every message.
    if (config.tx_buffer_bytes < wire::kMaxMessageSize || config.tx_buffer_bytes > (16U << 20)) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

}  // namespace

Status Gateway::init(const GatewayConfig& config) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    if (const Status status = validate(config); !ok(status)) {
        return status;
    }

    shard_by_symbol_.fill(kNoShard);
    shard_by_id_.fill(kNoShard);
    for (std::size_t i = 0; i < config.shards.size(); ++i) {
        const engine::Shard* const shard = config.shards[i];
        if (shard == nullptr || !shard->initialised()) {
            return Status::InvalidArgument;
        }
        const std::uint8_t id = shard->id().raw();
        if (id >= core::kMaxShards || shard_by_id_[id] != kNoShard) {
            return Status::Duplicate;
        }
        shard_by_id_[id] = static_cast<std::uint8_t>(i);
    }
    for (const Route& route : config.routes) {
        if (route.symbol.raw() >= core::kMaxSymbols || route.shard_index >= config.shards.size()) {
            return Status::OutOfRange;
        }
        // The gateway and the shard must agree on who trades what.
        if (config.shards[route.shard_index]->book(route.symbol) == nullptr) {
            return Status::NotFound;
        }
        shard_by_symbol_[route.symbol.raw()] = route.shard_index;
    }

    if (config.pin) {
        sys::HotThreadReport report;
        const sys::HotThreadConfig thread{.cpu = config.cpu, .name = "planck-gateway"};
        if (const Status status = sys::configure_hot_thread(thread, report); !ok(status)) {
            return status;
        }
    }

    // One receive and one transmit buffer per slot, touched now so the first
    // message on a new connection does not take a page fault.
    const std::size_t per_slot = kRxBufferBytes + config.tx_buffer_bytes;
    if (const Status status = arena_.init(kSlots * per_slot, sys::HugePages::Try); !ok(status)) {
        return status;
    }
    for (Connection& connection : connections_) {
        connection.rx = arena_.allocate(kRxBufferBytes, core::kCacheLine);
        connection.tx = arena_.allocate(config.tx_buffer_bytes, core::kCacheLine);
        if (connection.rx == nullptr || connection.tx == nullptr) {
            return Status::Capacity;
        }
    }
    arena_.prefault();
    slot_by_session_.fill(kNoConnection);

    if (const Status status = poller_.init(); !ok(status)) {
        return status;
    }
    net::Socket listener;
    if (const Status status = net::tcp_listen(config.listen, listener); !ok(status)) {
        return status;
    }
    if (const Status status = poller_.add(listener, kListenerToken); !ok(status)) {
        return status;
    }

    // Nobody is connected yet, whatever the shards remember from before a
    // restart. The rings are empty and this is their only producer, so
    // there is room.
    for (engine::Shard* const shard : config.shards) {
        if (!shard->commands().try_push(engine::make_all_sessions_closed())) {
            return Status::Capacity;
        }
    }

    shards_ = config.shards;
    max_sessions_ = config.max_sessions;
    credentials_ = config.credentials;
    latency_ = config.latency;
    tx_capacity_ = config.tx_buffer_bytes;
    idle_ = config.idle;
    listener_ = std::move(listener);
    return Status::Ok;
}

std::size_t Gateway::poll() noexcept {
    PLANCK_EXPECT(initialised());

    std::size_t work = 0;
    std::array<net::Event, net::Poller::kMaxEvents> events{};
    const std::size_t ready = poller_.wait(events, 0);
    for (std::size_t i = 0; i < ready; ++i) {
        const net::Event& event = events[i];
        if (event.token == kListenerToken) {
            work += accept_clients();
            continue;
        }
        Connection& connection = connections_[event.token & (kSlots - 1)];
        if (!connection.open) {
            continue;  // disconnected earlier in this same pass
        }
        if (event.readable) {
            work += read_connection(connection);
        }
        if (event.closed && connection.open) {
            disconnect(connection);
        }
    }

    if (owed_count_ != 0) [[unlikely]] {
        send_owed_closes();
    }
    work += route_reports();
    flush_dirty();
    return work;
}

void Gateway::run(const std::atomic<bool>& stop) noexcept {
    PLANCK_EXPECT(initialised());
    while (!stop.load(std::memory_order_relaxed)) {
        if (poll() == 0) {
            idle();
        }
    }
}

Gateway::Connection* Gateway::free_slot() noexcept {
    if (active_ >= max_sessions_) {
        return nullptr;
    }
    // Start after the slot handed out last, so a slot just vacated rests a
    // while before it is used again.
    for (std::size_t tries = 0; tries < kSlots; ++tries) {
        Connection& candidate = connections_[next_slot_];
        next_slot_ = (next_slot_ + 1) & (kSlots - 1);
        if (!candidate.open) {
            return &candidate;
        }
    }
    return nullptr;
}

std::size_t Gateway::accept_clients() noexcept {
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < kMaxAcceptsPerPoll; ++i) {
        net::Socket socket;
        net::Endpoint peer;
        if (!ok(net::tcp_accept(listener_, socket, peer))) {
            break;
        }
        ++accepted;
        ++stats_.connections;

        Connection* const connection = free_slot();
        if (connection == nullptr) {
            continue;  // full: the socket closes as it goes out of scope
        }
        if (!ok(poller_.add(socket, slot_of(*connection)))) {
            continue;
        }
        connection->socket = std::move(socket);
        connection->session = core::SessionId{};
        connection->open = true;
        connection->closing = false;
        connection->dirty = false;
        connection->rx_length = 0;
        connection->tx_length = 0;
        ++active_;
    }
    return accepted;
}

std::size_t Gateway::read_connection(Connection& connection) noexcept {
    PLANCK_EXPECT(connection.open);

    std::size_t reads = 0;
    for (std::size_t i = 0; i < kMaxReadsPerEvent; ++i) {
        PLANCK_ASSERT(connection.rx_length < kRxBufferBytes);
        const std::span<std::byte> space{connection.rx + connection.rx_length,
                                         kRxBufferBytes - connection.rx_length};
        const net::IoResult result = connection.socket.receive(space);
        if (result.status == Status::WouldBlock) {
            break;
        }
        if (!ok(result.status)) {
            disconnect(connection);
            break;
        }
        ++reads;
        if (connection.closing) {
            continue;  // on its way out: whatever it says now is not heard
        }
        connection.rx_length += static_cast<std::uint32_t>(result.bytes);
        if (!consume(connection)) {
            break;
        }
    }
    return reads;
}

bool Gateway::consume(Connection& connection) noexcept {
    // Stamp once per read. Every message in this batch arrived together.
    const std::uint64_t ingress_tsc = sys::tsc_now();
    std::uint32_t offset = 0;

    // Each pass consumes at least a header, so the buffer size bounds this.
    while (offset < connection.rx_length) {
        const wire::Frame frame =
            wire::next_frame({connection.rx + offset, connection.rx_length - offset});
        if (frame.status == Status::WouldBlock) {
            break;
        }
        if (!ok(frame.status) || !dispatch(connection, frame, ingress_tsc)) {
            ++stats_.protocol_errors;
            disconnect(connection);
            return false;
        }
        if (!connection.open || connection.closing) {
            return false;  // the message itself ended the connection
        }
        ++stats_.messages_in;
        offset += static_cast<std::uint32_t>(frame.bytes.size());
    }

    // Keep any partial message for the next read.
    connection.rx_length -= offset;
    if (connection.rx_length != 0 && offset != 0) {
        std::memmove(connection.rx, connection.rx + offset, connection.rx_length);
    }
    return true;
}

bool Gateway::dispatch(Connection& connection, const wire::Frame& frame,
                       std::uint64_t ingress_tsc) noexcept {
    switch (frame.type) {
        case wire::MsgType::LoginRequest: {
            wire::LoginRequest request;
            if (!ok(wire::decode(frame.bytes, request)) || connection.session.raw() != 0) {
                return false;
            }
            on_login(connection, request);
            return true;
        }
        case wire::MsgType::NewOrder: {
            wire::NewOrder order;
            if (!ok(wire::decode(frame.bytes, order))) {
                return false;
            }
            on_new_order(connection, order, ingress_tsc);
            return true;
        }
        case wire::MsgType::CancelOrder: {
            wire::CancelOrder cancel;
            if (!ok(wire::decode(frame.bytes, cancel))) {
                return false;
            }
            on_cancel(connection, cancel, ingress_tsc);
            return true;
        }
        case wire::MsgType::Heartbeat:
            queue_message(connection, wire::Heartbeat{});
            return true;
        case wire::MsgType::Logout:
            disconnect(connection);
            return true;
        default:
            // A well-formed message that a client has no business sending.
            return false;
    }
}

void Gateway::on_login(Connection& connection, const wire::LoginRequest& request) noexcept {
    wire::LoginRejected rejected{};
    bool refused = false;
    if (!wire::well_formed(request) || request.protocol_version != wire::kProtocolVersion) {
        rejected.reason = wire::LoginRejectReason::UnsupportedVersion;
        refused = true;
    } else if (request.client_id == 0 || request.client_id > kMaxSessionId ||
               (credentials_ != nullptr &&
                !credentials_->permits(request.client_id, request.token))) {
        // One answer for "no such client" and "wrong token", so a stranger
        // cannot learn which client ids exist.
        rejected.reason = wire::LoginRejectReason::BadCredentials;
        ++stats_.bad_credentials;
        refused = true;
    } else if (slot_by_session_[request.client_id] != kNoConnection ||
               close_owed_[request.client_id] != 0) {
        // Either it is connected, or the shards have not all heard that its
        // last connection ended.
        rejected.reason = wire::LoginRejectReason::AlreadyLoggedIn;
        refused = true;
    }
    if (refused) {
        // Told why, then dropped. Another try needs another connection.
        ++stats_.rejected;
        queue_message(connection, rejected);
        if (connection.open) {
            connection.closing = true;
        }
        return;
    }

    connection.session = core::SessionId{static_cast<std::uint16_t>(request.client_id)};
    slot_by_session_[connection.session.raw()] = slot_of(connection);
    ++stats_.logins;
    wire::LoginAccepted accepted{};
    accepted.session_id = connection.session.raw();
    queue_message(connection, accepted);
}

void Gateway::on_new_order(Connection& connection, const wire::NewOrder& order,
                           std::uint64_t ingress_tsc) noexcept {
    if (connection.session.raw() == 0) {
        reject_order(connection, order.client_order_id, wire::RejectCode::NotLoggedIn);
        return;
    }
    // The trust boundary. Nothing past this line re-checks these fields.
    if (!wire::well_formed(order)) {
        reject_order(connection, order.client_order_id, wire::RejectCode::Malformed);
        return;
    }
    const std::uint8_t index =
        (order.symbol_id < core::kMaxSymbols) ? shard_by_symbol_[order.symbol_id] : kNoShard;
    if (index == kNoShard) {
        reject_order(connection, order.client_order_id, wire::RejectCode::UnknownSymbol);
        return;
    }
    if (!shards_[index]->commands().try_push(engine::make_command(order, connection.session, ingress_tsc))) {
        ++stats_.busy;
        reject_order(connection, order.client_order_id, wire::RejectCode::Busy);
        return;
    }
    ++stats_.commands_out;
}

void Gateway::on_cancel(Connection& connection, const wire::CancelOrder& cancel,
                        std::uint64_t ingress_tsc) noexcept {
    if (connection.session.raw() == 0) {
        reject_cancel(connection, cancel.order_id, wire::RejectCode::NotLoggedIn);
        return;
    }
    if (!wire::well_formed(cancel)) {
        reject_cancel(connection, cancel.order_id, wire::RejectCode::Malformed);
        return;
    }
    // The order id names the shard that issued it, so no lookup is needed.
    const std::uint8_t shard_id =
        book::OrderStore::shard_of(core::OrderId{cancel.order_id}).raw();
    const std::uint8_t index = (shard_id < core::kMaxShards) ? shard_by_id_[shard_id] : kNoShard;
    if (index == kNoShard) {
        reject_cancel(connection, cancel.order_id, wire::RejectCode::UnknownOrder);
        return;
    }
    if (!shards_[index]->commands().try_push(engine::make_command(cancel, connection.session, ingress_tsc))) {
        ++stats_.busy;
        reject_cancel(connection, cancel.order_id, wire::RejectCode::Busy);
        return;
    }
    ++stats_.commands_out;
}

void Gateway::reject_order(Connection& connection, std::uint64_t client_order_id,
                           wire::RejectCode reason) noexcept {
    wire::OrderRejected message{};
    message.reason = reason;
    message.client_order_id = client_order_id;
    queue_message(connection, message);
    ++stats_.rejected;
}

void Gateway::reject_cancel(Connection& connection, std::uint64_t order_id,
                            wire::RejectCode reason) noexcept {
    wire::CancelRejected message{};
    message.reason = reason;
    message.order_id = order_id;
    queue_message(connection, message);
    ++stats_.rejected;
}

std::size_t Gateway::route_reports() noexcept {
    std::size_t routed = 0;
    for (engine::Shard* const shard : shards_) {
        // One counter read per batch. Every report in it is picked up now.
        const std::uint64_t now_tsc = (latency_ != nullptr) ? sys::tsc_now() : 0;
        routed += shard->reports().drain(kMaxReportsPerShard, [this, now_tsc](const engine::Report& r) noexcept {
            if (latency_ != nullptr && r.ingress_tsc != 0 && now_tsc >= r.ingress_tsc) {
                latency_->record(now_tsc - r.ingress_tsc);
            }
            Connection* const connection = find(r.session);
            if (connection == nullptr) {
                // Its owner has no connection just now.
                ++stats_.reports_dropped;
                return;
            }
            queue(*connection, {r.bytes.data(), r.length});
            ++stats_.reports_out;
        });
    }
    return routed;
}

template <wire::Message T>
void Gateway::queue_message(Connection& connection, const T& message) noexcept {
    queue(connection, {reinterpret_cast<const std::byte*>(&message), sizeof(T)});
}

void Gateway::queue(Connection& connection, std::span<const std::byte> bytes) noexcept {
    PLANCK_EXPECT(connection.open);
    PLANCK_EXPECT(!bytes.empty() && bytes.size() <= wire::kMaxMessageSize);

    if (bytes.size() > (tx_capacity_ - connection.tx_length)) {
        // The client is not reading. Holding its backlog would mean either
        // unbounded memory or stalling everyone else, so it goes.
        ++stats_.slow_consumers;
        disconnect(connection);
        return;
    }
    std::memcpy(connection.tx + connection.tx_length, bytes.data(), bytes.size());
    connection.tx_length += static_cast<std::uint32_t>(bytes.size());
    if (!connection.dirty) {
        connection.dirty = true;
        PLANCK_ASSERT(dirty_count_ < dirty_slots_.size());
        dirty_slots_[dirty_count_] = slot_of(connection);
        ++dirty_count_;
    }
}

void Gateway::flush_dirty() noexcept {
    // One send per connection per pass, however many messages were queued.
    std::size_t still_dirty = 0;
    for (std::size_t i = 0; i < dirty_count_; ++i) {
        Connection& connection = connections_[dirty_slots_[i]];
        if (!connection.open || !connection.dirty) {
            continue;  // disconnected since it was queued
        }
        if (connection.tx_length != 0) {
            const net::IoResult result = connection.socket.send({connection.tx, connection.tx_length});
            if (ok(result.status)) {
                const auto sent = static_cast<std::uint32_t>(result.bytes);
                connection.tx_length -= sent;
                if (connection.tx_length != 0) {
                    std::memmove(connection.tx, connection.tx + sent, connection.tx_length);
                }
            } else if (result.status != Status::WouldBlock) {
                disconnect(connection);
                continue;
            }
        }
        if (connection.tx_length == 0) {
            connection.dirty = false;
            if (connection.closing) {
                disconnect(connection);  // it has been told why
            }
        } else {
            // The socket would not take it all. Try again next pass.
            dirty_slots_[still_dirty] = dirty_slots_[i];
            ++still_dirty;
        }
    }
    dirty_count_ = still_dirty;
}

void Gateway::disconnect(Connection& connection) noexcept {
    PLANCK_EXPECT(connection.open);
    PLANCK_EXPECT(active_ > 0);

    // The session lives on without a connection, and the shards are told.
    const core::SessionId session = connection.session;
    if (session.raw() != 0) {
        PLANCK_ASSERT(slot_by_session_[session.raw()] == slot_of(connection));
        slot_by_session_[session.raw()] = kNoConnection;
        close_session(session);
    }
    // The poller forgets a descriptor when it is closed, but say so anyway.
    static_cast<void>(poller_.remove(connection.socket));
    connection.socket.close();
    connection.session = core::SessionId{};
    connection.open = false;
    connection.closing = false;
    connection.dirty = false;
    connection.rx_length = 0;
    connection.tx_length = 0;
    --active_;
    ++stats_.disconnects;
}

void Gateway::close_session(core::SessionId session) noexcept {
    PLANCK_EXPECT(session.raw() != 0);
    PLANCK_EXPECT(close_owed_[session.raw()] == 0);

    std::uint16_t owed = 0;
    for (std::size_t i = 0; i < shards_.size(); ++i) {
        ++stats_.session_closes;
        if (!shards_[i]->commands().try_push(engine::make_session_closed(session))) {
            owed = static_cast<std::uint16_t>(owed | (1U << i));
            ++stats_.session_closes_delayed;
        }
    }
    if (owed != 0) [[unlikely]] {
        // A session is listed at most once: it cannot log in, and so cannot
        // close again, until this is settled.
        PLANCK_ASSERT(owed_count_ < owed_sessions_.size());
        close_owed_[session.raw()] = owed;
        owed_sessions_[owed_count_] = session.raw();
        ++owed_count_;
    }
}

void Gateway::send_owed_closes() noexcept {
    std::size_t still_owed = 0;
    for (std::size_t i = 0; i < owed_count_; ++i) {
        const std::uint16_t session = owed_sessions_[i];
        std::uint16_t owed = close_owed_[session];
        for (std::size_t shard = 0; shard < shards_.size(); ++shard) {
            const auto bit = static_cast<std::uint16_t>(1U << shard);
            if ((owed & bit) != 0 &&
                shards_[shard]->commands().try_push(
                    engine::make_session_closed(core::SessionId{session}))) {
                owed = static_cast<std::uint16_t>(owed & ~bit);
            }
        }
        close_owed_[session] = owed;
        if (owed != 0) {
            owed_sessions_[still_owed] = session;
            ++still_owed;
        }
    }
    owed_count_ = still_owed;
}

void Gateway::idle() const noexcept {
    if (idle_ == engine::IdlePolicy::Spin) {
        core::cpu_relax();
    } else {
        static_cast<void>(::sched_yield());
    }
}

}  // namespace planck::gateway
