#ifndef PLANCK_TESTS_SUPPORT_FEED_AUDITOR_HPP
#define PLANCK_TESTS_SUPPORT_FEED_AUDITOR_HPP

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "planck/engine/shard.hpp"
#include "support/shard_harness.hpp"

namespace planck::test {

// Watches everything a shard emits and checks it against two independent
// pictures of the world:
//
//   public   books rebuilt from market data alone, the way a subscriber would
//   private  each order's state as told to its owner through reports
//
// Every message is checked as it arrives, and the two pictures are checked
// against each other and against the shard's real books on demand. The
// auditor knows the protocol's rules but shares no code with the engine.
class FeedAuditor {
public:
    struct Candidate {
        std::uint64_t order_id = 0;
        std::uint16_t session = 0;
    };

    // Tell the auditor a NewOrder is on its way, so it can match the reply.
    void expect_reply(std::uint64_t client_order_id, std::uint16_t session, std::uint32_t qty) {
        unanswered_[client_order_id] = Pending{session, qty};
    }

    // Tell the auditor that a session, or every session, is about to be
    // closed. Only then may orders be cancelled for that reason.
    void expect_close(std::uint16_t session) { closed_sessions_.insert(session); }
    void expect_close_of_all() { all_closed_ = true; }

    // Nothing may be live for a session once its close has been carried out.
    // Call only when everything the shard emitted has been seen.
    void expect_nothing_live_for(std::uint16_t session) const {
        for (const auto& [id, order] : private_orders_) {
            EXPECT_FALSE(order.live && order.session == session)
                << "order " << id << " outlived the connection of session " << session;
        }
    }
    void expect_nothing_live() const {
        for (const auto& [id, order] : private_orders_) {
            EXPECT_FALSE(order.live) << "order " << id << " outlived every connection";
        }
        EXPECT_TRUE(public_orders_.empty()) << "the feed still shows orders";
    }
    [[nodiscard]] std::uint64_t disconnect_cancels() const { return disconnect_cancels_; }

    void on_market(const engine::MarketEvent& event) {
        ++market_count_;
        switch (type_of(event)) {
            case wire::MsgType::MdAddOrder:
                on_add(as<wire::MdAddOrder>(event));
                break;
            case wire::MsgType::MdExecuted:
                on_print(as<wire::MdExecuted>(event));
                break;
            case wire::MsgType::MdDeleted:
                on_delete(as<wire::MdDeleted>(event));
                break;
            default:
                ADD_FAILURE() << "not a market data message";
        }
    }

    void on_report(const engine::Report& report) {
        ++report_count_;
        const std::uint16_t session = report.session.raw();
        switch (type_of(report)) {
            case wire::MsgType::OrderAccepted:
                on_accepted(as<wire::OrderAccepted>(report), session);
                break;
            case wire::MsgType::OrderRejected:
                on_rejected(as<wire::OrderRejected>(report), session);
                break;
            case wire::MsgType::Executed:
                on_executed(as<wire::Executed>(report), session);
                break;
            case wire::MsgType::Canceled:
                on_canceled(as<wire::Canceled>(report), session);
                break;
            case wire::MsgType::CancelRejected:
                on_cancel_rejected(as<wire::CancelRejected>(report), session);
                break;
            default:
                ADD_FAILURE() << "not an order entry report";
        }
    }

    // Every order ever accepted, including ones long gone. Cancelling a dead
    // order is a perfectly good test input.
    [[nodiscard]] std::vector<Candidate>& cancel_candidates() { return candidates_; }
    [[nodiscard]] std::size_t unanswered() const { return unanswered_.size(); }
    [[nodiscard]] std::uint64_t messages_seen() const { return report_count_ + market_count_; }
    [[nodiscard]] std::size_t resting_orders() const { return public_orders_.size(); }

    // The public and private pictures must describe the same resting orders.
    void expect_views_agree() const {
        EXPECT_TRUE(half_reported_matches_.empty()) << "a fill was reported to only one side";
        EXPECT_EQ(taker_qty_, maker_qty_);
        EXPECT_EQ(taker_qty_, printed_qty_) << "private fills and public prints disagree";

        std::size_t live = 0;
        for (const auto& [id, order] : private_orders_) {
            if (!order.live) {
                continue;
            }
            ++live;
            const auto shown = public_orders_.find(id);
            if (shown == public_orders_.end()) {
                ADD_FAILURE() << "order " << id << " is live for its owner but not on the feed";
                continue;
            }
            EXPECT_EQ(shown->second.qty, order.open) << "order " << id;
        }
        EXPECT_EQ(live, public_orders_.size());
    }

    // The books rebuilt from the feed must equal the shard's real books at
    // every price. Call only while the shard is not running.
    void expect_matches(const engine::Shard& shard,
                        std::span<const engine::SymbolConfig> symbols) const {
        EXPECT_EQ(shard.store().in_use(), public_orders_.size());
        for (const engine::SymbolConfig& symbol : symbols) {
            const book::OrderBook* const book = shard.book(symbol.symbol);
            ASSERT_NE(book, nullptr);
            const std::int32_t low = symbol.band_low.raw();
            const std::int32_t high = low + static_cast<std::int32_t>(symbol.band_ticks);
            for (std::int32_t price = low; price < high; ++price) {
                const auto level = depth_.find({symbol.symbol.raw(), price});
                const std::uint64_t expected = (level == depth_.end()) ? 0 : level->second;
                ASSERT_EQ(book->quantity_at(core::Price{price}), expected)
                    << "symbol " << symbol.symbol.raw() << " price " << price;
            }
        }
    }

private:
    struct Pending {
        std::uint16_t session = 0;
        std::uint32_t qty = 0;
    };
    struct PublicOrder {
        std::uint16_t symbol = 0;
        std::int32_t price = 0;
        std::uint32_t qty = 0;
    };
    struct PrivateOrder {
        std::uint16_t session = 0;
        std::uint32_t open = 0;
        bool live = false;
    };
    struct HalfMatch {
        std::uint32_t qty = 0;
        wire::Liquidity seen = wire::Liquidity::Maker;
    };

    void remove_depth(const PublicOrder& order, std::uint32_t qty) {
        const auto level = depth_.find({order.symbol, order.price});
        ASSERT_NE(level, depth_.end());
        ASSERT_GE(level->second, qty);
        level->second -= qty;
        if (level->second == 0) {
            depth_.erase(level);
        }
    }

    void on_add(const wire::MdAddOrder& message) {
        EXPECT_GT(message.qty, 0U);
        EXPECT_FALSE(public_orders_.contains(message.order_id)) << "order added to the feed twice";
        public_orders_[message.order_id] = PublicOrder{message.symbol_id, message.price, message.qty};
        depth_[{message.symbol_id, message.price}] += message.qty;
    }

    void on_print(const wire::MdExecuted& message) {
        const auto order = public_orders_.find(message.order_id);
        ASSERT_NE(order, public_orders_.end()) << "print for an order the feed never showed";
        EXPECT_EQ(order->second.price, message.price) << "trades happen at the resting price";
        EXPECT_EQ(order->second.symbol, message.symbol_id);
        EXPECT_GT(message.match_id, last_match_id_) << "match ids must increase";
        ASSERT_GE(order->second.qty, message.qty) << "print larger than the order";
        last_match_id_ = message.match_id;
        printed_qty_ += message.qty;

        remove_depth(order->second, message.qty);
        order->second.qty -= message.qty;
        if (order->second.qty == 0) {
            public_orders_.erase(order);
        }
    }

    void on_delete(const wire::MdDeleted& message) {
        const auto order = public_orders_.find(message.order_id);
        ASSERT_NE(order, public_orders_.end()) << "delete for an order the feed never showed";
        EXPECT_EQ(order->second.symbol, message.symbol_id);
        remove_depth(order->second, order->second.qty);
        public_orders_.erase(order);
    }

    void on_accepted(const wire::OrderAccepted& message, std::uint16_t session) {
        const auto pending = unanswered_.find(message.client_order_id);
        ASSERT_NE(pending, unanswered_.end()) << "acceptance for an order nobody sent";
        EXPECT_EQ(pending->second.session, session) << "acceptance sent to the wrong session";
        EXPECT_FALSE(private_orders_.contains(message.order_id)) << "order id issued twice";
        private_orders_[message.order_id] = PrivateOrder{session, pending->second.qty, true};
        candidates_.push_back(Candidate{message.order_id, session});
        unanswered_.erase(pending);
    }

    void on_rejected(const wire::OrderRejected& message, std::uint16_t session) {
        const auto pending = unanswered_.find(message.client_order_id);
        ASSERT_NE(pending, unanswered_.end()) << "rejection for an order nobody sent";
        EXPECT_EQ(pending->second.session, session);
        EXPECT_NE(message.reason, wire::RejectCode::None);
        unanswered_.erase(pending);
    }

    void on_executed(const wire::Executed& message, std::uint16_t session) {
        const auto order = private_orders_.find(message.order_id);
        ASSERT_NE(order, private_orders_.end()) << "fill reported before the order was accepted";
        EXPECT_EQ(order->second.session, session) << "fill sent to the wrong session";
        EXPECT_TRUE(order->second.live) << "fill on an order that was already done";
        EXPECT_GT(message.qty, 0U);
        ASSERT_GE(order->second.open, message.qty) << "filled more than was open";
        order->second.open -= message.qty;
        EXPECT_EQ(order->second.open, message.leaves);
        order->second.live = (message.leaves != 0);

        // Each match must be reported exactly twice: once to the maker and
        // once to the taker, for the same quantity.
        const auto half = half_reported_matches_.find(message.match_id);
        if (half == half_reported_matches_.end()) {
            half_reported_matches_[message.match_id] = HalfMatch{message.qty, message.liquidity};
        } else {
            EXPECT_EQ(half->second.qty, message.qty);
            EXPECT_NE(half->second.seen, message.liquidity) << "both reports claim the same side";
            half_reported_matches_.erase(half);
        }
        (message.liquidity == wire::Liquidity::Taker ? taker_qty_ : maker_qty_) += message.qty;
    }

    void on_canceled(const wire::Canceled& message, std::uint16_t session) {
        const auto order = private_orders_.find(message.order_id);
        ASSERT_NE(order, private_orders_.end()) << "cancel reported for an unknown order";
        EXPECT_EQ(order->second.session, session);
        EXPECT_TRUE(order->second.live);
        EXPECT_GT(message.qty, 0U);
        EXPECT_EQ(order->second.open, message.qty) << "cancelled quantity is not what was open";
        if (message.reason == wire::CancelReason::Disconnected) {
            EXPECT_TRUE(all_closed_ || closed_sessions_.contains(session))
                << "order " << message.order_id << " cancelled for a disconnect nobody had";
            ++disconnect_cancels_;
        }
        order->second.open = 0;
        order->second.live = false;
    }

    void on_cancel_rejected(const wire::CancelRejected& message, std::uint16_t session) {
        const auto order = private_orders_.find(message.order_id);
        const bool cancellable = (order != private_orders_.end()) && order->second.live &&
                                 (order->second.session == session);
        EXPECT_FALSE(cancellable) << "refused to cancel a live order for its own session";
    }

    std::unordered_map<std::uint64_t, Pending> unanswered_;
    std::unordered_map<std::uint64_t, PublicOrder> public_orders_;
    std::unordered_map<std::uint64_t, PrivateOrder> private_orders_;
    std::unordered_map<std::uint64_t, HalfMatch> half_reported_matches_;
    std::map<std::pair<std::uint16_t, std::int32_t>, std::uint64_t> depth_;
    std::vector<Candidate> candidates_;
    std::set<std::uint16_t> closed_sessions_;
    bool all_closed_ = false;
    std::uint64_t disconnect_cancels_ = 0;
    std::uint64_t last_match_id_ = 0;
    std::uint64_t taker_qty_ = 0;
    std::uint64_t maker_qty_ = 0;
    std::uint64_t printed_qty_ = 0;
    std::uint64_t report_count_ = 0;
    std::uint64_t market_count_ = 0;
};

// Produces a stream of plausible and implausible commands: mostly passive
// orders around a mid price, some that cross, cancels of live and dead
// orders, cancels from the wrong session, unknown symbols, zero quantities
// and prices outside the band. Now and then a session loses its connection,
// and once in a long while they all do.
class RandomFlow {
public:
    explicit RandomFlow(std::uint64_t seed) : rng_(seed) {}

    engine::Command next(FeedAuditor& auditor) {
        const auto roll = static_cast<unsigned>(rng_() % 100);
        std::vector<FeedAuditor::Candidate>& candidates = auditor.cancel_candidates();
        if (roll < 38 && !candidates.empty()) {
            const std::size_t pick = rng_() % candidates.size();
            const FeedAuditor::Candidate chosen = candidates[pick];
            candidates[pick] = candidates.back();
            candidates.pop_back();
            const bool impostor = (rng_() % 10 == 0);
            const auto session = static_cast<std::uint16_t>(chosen.session + (impostor ? 1 : 0));
            return cancel_order(session, chosen.order_id);
        }
        if (roll < 40) {
            return cancel_order(static_cast<std::uint16_t>(rng_() % kSessions), rng_());
        }
        if (roll == 99 && rng_() % 4 == 0) {
            if (rng_() % 40 == 0) {
                auditor.expect_close_of_all();
                return engine::make_all_sessions_closed();
            }
            const auto session = static_cast<std::uint16_t>(rng_() % kSessions);
            auditor.expect_close(session);
            return session_closed(session);
        }
        return random_new_order(auditor);
    }

private:
    static constexpr unsigned kSessions = 8;

    engine::Command random_new_order(FeedAuditor& auditor) {
        const engine::SymbolConfig& symbol = kSymbols[rng_() % kSymbols.size()];
        const bool buy = (rng_() % 2 == 0);
        const std::int32_t mid = symbol.band_low.raw() + static_cast<std::int32_t>(symbol.band_ticks / 2);
        const auto away = static_cast<std::int32_t>(rng_() % 30);

        // 70% rest away from the mid, 30% land near it and may cross.
        std::int32_t price = (rng_() % 100 < 70) ? (buy ? mid - 1 - away : mid + 1 + away)
                                                 : mid + static_cast<std::int32_t>(rng_() % 21) - 10;
        if (rng_() % 150 == 0) {
            price += 500;  // outside the band
        }
        const std::uint16_t symbol_id = (rng_() % 200 == 0) ? std::uint16_t{55} : symbol.symbol.raw();
        const std::uint32_t lots = static_cast<std::uint32_t>(1 + rng_() % 50);
        const std::uint32_t qty = (rng_() % 250 == 0) ? 0 : lots * ((rng_() % 40 == 0) ? 60U : 1U);
        const auto session = static_cast<std::uint16_t>(rng_() % kSessions);

        const auto kind = static_cast<unsigned>(rng_() % 100);
        const core::TimeInForce tif = (kind < 75)   ? core::TimeInForce::Day
                                      : (kind < 92) ? core::TimeInForce::ImmediateOrCancel
                                                    : core::TimeInForce::FillOrKill;
        const core::OrderType type = (kind >= 97) ? core::OrderType::Market : core::OrderType::Limit;

        const std::uint64_t client_order_id = next_client_order_id_++;
        auditor.expect_reply(client_order_id, session, qty);
        return new_order(session, symbol_id, buy ? core::Side::Buy : core::Side::Sell, price, qty,
                         client_order_id, tif, type);
    }

    std::mt19937_64 rng_;
    std::uint64_t next_client_order_id_ = 1;
};

}  // namespace planck::test

#endif  // PLANCK_TESTS_SUPPORT_FEED_AUDITOR_HPP
