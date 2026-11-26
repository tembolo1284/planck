#include "planck/gateway/gateway.hpp"

#include <algorithm>
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
    // A session must at least be able to hold one of every message.
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
    for (Session& session : sessions_) {
        session.rx = arena_.allocate(kRxBufferBytes, core::kCacheLine);
        session.tx = arena_.allocate(config.tx_buffer_bytes, core::kCacheLine);
        if (session.rx == nullptr || session.tx == nullptr) {
            return Status::Capacity;
        }
    }
    arena_.prefault();

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

    shards_ = config.shards;
    max_sessions_ = config.max_sessions;
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
        Session& session = sessions_[event.token & (kSlots - 1)];
        if (session.id.raw() == 0) {
            continue;  // disconnected earlier in this same pass
        }
        if (event.readable) {
            work += read_session(session);
        }
        if (event.closed && session.id.raw() != 0) {
            disconnect(session);
        }
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

        // Find the next id whose slot is free. Ids only ever go up, so an id
        // whose slot is still busy is skipped for good.
        Session* session = nullptr;
        for (std::size_t tries = 0; tries < kSlots && active_ < max_sessions_; ++tries) {
            if (next_session_id_ > kMaxSessionId) {
                break;
            }
            const core::SessionId id{static_cast<std::uint16_t>(next_session_id_)};
            ++next_session_id_;
            if (sessions_[slot_of(id)].id.raw() == 0) {
                session = &sessions_[slot_of(id)];
                session->id = id;
                break;
            }
        }
        if (session == nullptr) {
            continue;  // full, or out of ids: socket closes as it goes out of scope
        }
        if (!ok(poller_.add(socket, slot_of(session->id)))) {
            session->id = core::SessionId{};
            continue;
        }
        session->socket = std::move(socket);
        session->client_id = 0;
        session->logged_in = false;
        session->dirty = false;
        session->rx_length = 0;
        session->tx_length = 0;
        ++active_;
    }
    return accepted;
}

std::size_t Gateway::read_session(Session& session) noexcept {
    PLANCK_EXPECT(session.id.raw() != 0);

    std::size_t reads = 0;
    for (std::size_t i = 0; i < kMaxReadsPerEvent; ++i) {
        PLANCK_ASSERT(session.rx_length < kRxBufferBytes);
        const std::span<std::byte> space{session.rx + session.rx_length,
                                         kRxBufferBytes - session.rx_length};
        const net::IoResult result = session.socket.receive(space);
        if (result.status == Status::WouldBlock) {
            break;
        }
        if (!ok(result.status)) {
            disconnect(session);
            break;
        }
        ++reads;
        session.rx_length += static_cast<std::uint32_t>(result.bytes);
        if (!consume(session)) {
            break;
        }
    }
    return reads;
}

bool Gateway::consume(Session& session) noexcept {
    // Stamp once per read. Every message in this batch arrived together.
    const std::uint64_t ingress_tsc = sys::tsc_now();
    std::uint32_t offset = 0;

    // Each pass consumes at least a header, so the buffer size bounds this.
    while (offset < session.rx_length) {
        const wire::Frame frame =
            wire::next_frame({session.rx + offset, session.rx_length - offset});
        if (frame.status == Status::WouldBlock) {
            break;
        }
        if (!ok(frame.status) || !dispatch(session, frame, ingress_tsc)) {
            ++stats_.protocol_errors;
            disconnect(session);
            return false;
        }
        if (session.id.raw() == 0) {
            return false;  // the message itself ended the session
        }
        ++stats_.messages_in;
        offset += static_cast<std::uint32_t>(frame.bytes.size());
    }

    // Keep any partial message for the next read.
    session.rx_length -= offset;
    if (session.rx_length != 0 && offset != 0) {
        std::memmove(session.rx, session.rx + offset, session.rx_length);
    }
    return true;
}

bool Gateway::dispatch(Session& session, const wire::Frame& frame,
                       std::uint64_t ingress_tsc) noexcept {
    switch (frame.type) {
        case wire::MsgType::LoginRequest: {
            wire::LoginRequest request;
            if (!ok(wire::decode(frame.bytes, request)) || session.logged_in) {
                return false;
            }
            on_login(session, request);
            return true;
        }
        case wire::MsgType::NewOrder: {
            wire::NewOrder order;
            if (!ok(wire::decode(frame.bytes, order))) {
                return false;
            }
            on_new_order(session, order, ingress_tsc);
            return true;
        }
        case wire::MsgType::CancelOrder: {
            wire::CancelOrder cancel;
            if (!ok(wire::decode(frame.bytes, cancel))) {
                return false;
            }
            on_cancel(session, cancel, ingress_tsc);
            return true;
        }
        case wire::MsgType::Heartbeat:
            queue_message(session, wire::Heartbeat{});
            return true;
        case wire::MsgType::Logout:
            disconnect(session);
            return true;
        default:
            // A well-formed message that a client has no business sending.
            return false;
    }
}

void Gateway::on_login(Session& session, const wire::LoginRequest& request) noexcept {
    wire::LoginRejected rejected{};
    bool refused = false;
    if (!wire::well_formed(request) || request.protocol_version != wire::kProtocolVersion) {
        rejected.reason = wire::LoginRejectReason::UnsupportedVersion;
        refused = true;
    } else {
        // Logins are rare, so a scan of the table is fine here.
        const bool taken = std::ranges::any_of(sessions_, [&](const Session& other) {
            return other.logged_in && other.client_id == request.client_id;
        });
        if (taken) {
            rejected.reason = wire::LoginRejectReason::AlreadyLoggedIn;
            refused = true;
        }
    }
    if (refused) {
        queue_message(session, rejected);
        ++stats_.rejected;
        return;
    }

    session.client_id = request.client_id;
    session.logged_in = true;
    ++stats_.logins;
    wire::LoginAccepted accepted{};
    accepted.session_id = session.id.raw();
    queue_message(session, accepted);
}

void Gateway::on_new_order(Session& session, const wire::NewOrder& order,
                           std::uint64_t ingress_tsc) noexcept {
    if (!session.logged_in) {
        reject_order(session, order.client_order_id, wire::RejectCode::NotLoggedIn);
        return;
    }
    // The trust boundary. Nothing past this line re-checks these fields.
    if (!wire::well_formed(order)) {
        reject_order(session, order.client_order_id, wire::RejectCode::Malformed);
        return;
    }
    const std::uint8_t index =
        (order.symbol_id < core::kMaxSymbols) ? shard_by_symbol_[order.symbol_id] : kNoShard;
    if (index == kNoShard) {
        reject_order(session, order.client_order_id, wire::RejectCode::UnknownSymbol);
        return;
    }
    if (!shards_[index]->commands().try_push(engine::make_command(order, session.id, ingress_tsc))) {
        ++stats_.busy;
        reject_order(session, order.client_order_id, wire::RejectCode::Busy);
        return;
    }
    ++stats_.commands_out;
}

void Gateway::on_cancel(Session& session, const wire::CancelOrder& cancel,
                        std::uint64_t ingress_tsc) noexcept {
    if (!session.logged_in) {
        reject_cancel(session, cancel.order_id, wire::RejectCode::NotLoggedIn);
        return;
    }
    if (!wire::well_formed(cancel)) {
        reject_cancel(session, cancel.order_id, wire::RejectCode::Malformed);
        return;
    }
    // The order id names the shard that issued it, so no lookup is needed.
    const std::uint8_t shard_id =
        book::OrderStore::shard_of(core::OrderId{cancel.order_id}).raw();
    const std::uint8_t index = (shard_id < core::kMaxShards) ? shard_by_id_[shard_id] : kNoShard;
    if (index == kNoShard) {
        reject_cancel(session, cancel.order_id, wire::RejectCode::UnknownOrder);
        return;
    }
    if (!shards_[index]->commands().try_push(engine::make_command(cancel, session.id, ingress_tsc))) {
        ++stats_.busy;
        reject_cancel(session, cancel.order_id, wire::RejectCode::Busy);
        return;
    }
    ++stats_.commands_out;
}

void Gateway::reject_order(Session& session, std::uint64_t client_order_id,
                           wire::RejectCode reason) noexcept {
    wire::OrderRejected message{};
    message.reason = reason;
    message.client_order_id = client_order_id;
    queue_message(session, message);
    ++stats_.rejected;
}

void Gateway::reject_cancel(Session& session, std::uint64_t order_id,
                            wire::RejectCode reason) noexcept {
    wire::CancelRejected message{};
    message.reason = reason;
    message.order_id = order_id;
    queue_message(session, message);
    ++stats_.rejected;
}

std::size_t Gateway::route_reports() noexcept {
    std::size_t routed = 0;
    for (engine::Shard* const shard : shards_) {
        routed += shard->reports().drain(kMaxReportsPerShard, [this](const engine::Report& r) noexcept {
            Session* const session = find(r.session);
            if (session == nullptr || !session->logged_in) {
                // The client left while its order was still working.
                ++stats_.reports_dropped;
                return;
            }
            queue(*session, {r.bytes.data(), r.length});
            ++stats_.reports_out;
        });
    }
    return routed;
}

template <wire::Message T>
void Gateway::queue_message(Session& session, const T& message) noexcept {
    queue(session, {reinterpret_cast<const std::byte*>(&message), sizeof(T)});
}

void Gateway::queue(Session& session, std::span<const std::byte> bytes) noexcept {
    PLANCK_EXPECT(session.id.raw() != 0);
    PLANCK_EXPECT(!bytes.empty() && bytes.size() <= wire::kMaxMessageSize);

    if (bytes.size() > (tx_capacity_ - session.tx_length)) {
        // The client is not reading. Holding its backlog would mean either
        // unbounded memory or stalling everyone else, so it goes.
        ++stats_.slow_consumers;
        disconnect(session);
        return;
    }
    std::memcpy(session.tx + session.tx_length, bytes.data(), bytes.size());
    session.tx_length += static_cast<std::uint32_t>(bytes.size());
    if (!session.dirty) {
        session.dirty = true;
        PLANCK_ASSERT(dirty_count_ < dirty_slots_.size());
        dirty_slots_[dirty_count_] = static_cast<std::uint16_t>(slot_of(session.id));
        ++dirty_count_;
    }
}

void Gateway::flush_dirty() noexcept {
    // One send per session per pass, however many messages were queued.
    std::size_t still_dirty = 0;
    for (std::size_t i = 0; i < dirty_count_; ++i) {
        Session& session = sessions_[dirty_slots_[i]];
        if (session.id.raw() == 0 || !session.dirty) {
            continue;  // disconnected since it was queued
        }
        if (session.tx_length != 0) {
            const net::IoResult result = session.socket.send({session.tx, session.tx_length});
            if (ok(result.status)) {
                const auto sent = static_cast<std::uint32_t>(result.bytes);
                session.tx_length -= sent;
                if (session.tx_length != 0) {
                    std::memmove(session.tx, session.tx + sent, session.tx_length);
                }
            } else if (result.status != Status::WouldBlock) {
                disconnect(session);
                continue;
            }
        }
        if (session.tx_length == 0) {
            session.dirty = false;
        } else {
            // The socket would not take it all. Try again next pass.
            dirty_slots_[still_dirty] = dirty_slots_[i];
            ++still_dirty;
        }
    }
    dirty_count_ = still_dirty;
}

void Gateway::disconnect(Session& session) noexcept {
    PLANCK_EXPECT(session.id.raw() != 0);
    PLANCK_EXPECT(active_ > 0);

    // The poller forgets a descriptor when it is closed, but say so anyway.
    static_cast<void>(poller_.remove(session.socket));
    session.socket.close();
    session.id = core::SessionId{};
    session.client_id = 0;
    session.logged_in = false;
    session.dirty = false;
    session.rx_length = 0;
    session.tx_length = 0;
    --active_;
    ++stats_.disconnects;
}

void Gateway::idle() const noexcept {
    if (idle_ == engine::IdlePolicy::Spin) {
        core::cpu_relax();
    } else {
        static_cast<void>(::sched_yield());
    }
}

}  // namespace planck::gateway
