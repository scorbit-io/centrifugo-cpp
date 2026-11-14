#pragma once

#include <memory>
#include <unordered_map>

#include <boost/asio/steady_timer.hpp>
#include <boost/signals2/signal.hpp>

#include <centrifugo/subscription.h>
#include "transport.h"

namespace centrifugo {

class SubscriptionImpl
{
public:
    using SubscribingSignal = boost::signals2::signal<void()>;
    using SubscribedSignal = boost::signals2::signal<void()>;
    using UnsubscribedSignal = boost::signals2::signal<void()>;
    using PublicationSignal = boost::signals2::signal<void(Publication const &)>;
    using ErrorSignal = boost::signals2::signal<void(Error const &)>;

    SubscriptionImpl(std::string const &channel, Transport &transport,
                     SubscriptionConfig config = {});
    ~SubscriptionImpl();

    // Never moved: built in place in a node-based map, and timer handlers capture `this`.
    SubscriptionImpl(SubscriptionImpl const &) = delete;
    auto operator=(SubscriptionImpl const &) -> SubscriptionImpl & = delete;
    SubscriptionImpl(SubscriptionImpl &&) = delete;
    auto operator=(SubscriptionImpl &&) -> SubscriptionImpl & = delete;

    auto state() const -> SubscriptionState;
    auto channel() const -> std::string const &;
    auto subscription() -> Subscription &;

    auto subscribe() -> outcome::result<void, std::string>;
    auto unsubscribe() -> void;
    auto publish(nlohmann::json const &json) -> outcome::result<void, Error>;
    auto history(HistoryOptions const &options, Subscription::HistoryCallback onResult)
            -> outcome::result<void, Error>;

    auto handleReply(Reply const &reply) -> bool;
    auto handlePublish(Publication const &publication) -> void;
    auto handleServerUnsubscribe(Unsubscribe const &unsubscribe) -> void;
    // Removed from the client: stop reacting to the connection; destruction follows later.
    auto detach() -> void;

    auto onSubscribing() -> SubscribingSignal &;
    auto onSubscribed() -> SubscribedSignal &;
    auto onUnsubscribed() -> UnsubscribedSignal &;
    auto onPublication() -> PublicationSignal &;
    auto onError() -> ErrorSignal &;

private:
    // What a command awaiting its reply was, to know what its reply means. Abandoned: sent for
    // a subscription the app has since ended; its reply is dropped.
    enum class CommandKind { Subscribe, Unsubscribe, Publish, SubRefresh, Abandoned };
    enum class TimerAction { Refresh, Resubscribe };

    auto init() -> void;
    auto deinit() -> void;
    auto sendCmd(Command &&cmd, CommandKind kind) -> void;
    auto sendSubscribeCmd() -> void;
    auto setState(SubscriptionState newState) -> void;
    auto abandonPending() -> void;
    auto serverMayHaveIt() const -> bool;

    auto usesToken() const -> bool;
    auto fetchToken() -> outcome::result<std::string, Error>;
    auto handleSubscribed(SubscribeResult const &result) -> void;
    auto handleSubscribeError(ErrorReply const &error) -> void;
    auto handleRefreshError(ErrorReply const &error) -> void;
    auto endSubscription(Error const &error) -> void;
    auto startTimer(std::chrono::seconds delay, TimerAction action) -> void;
    auto startRefreshTimer(std::uint32_t ttlSeconds) -> void;
    auto refreshToken() -> void;
    auto failPendingHistory(std::string const &reason) -> void;

private:
    std::string channel_;
    Transport &transport_;
    SubscriptionConfig config_;

    Subscription subscription_;
    SubscriptionState state_ = SubscriptionState::UNSUBSCRIBED;
    std::unordered_map<std::uint32_t, CommandKind> waitingReplies_;
    // Command id -> result callback of a history read awaiting its reply.
    std::unordered_map<std::uint32_t, Subscription::HistoryCallback> pendingHistory_;

    // Stream recovery state
    std::string epoch_;
    std::uint64_t offset_ {0};
    bool recoverable_ {false};

    // Subscription token; a 109 to subscribe or sub_refresh is retried once with a fresh one.
    std::string token_;
    bool tokenRetried_ {false};
    bool refreshRetried_ {false};
    // Bumped on every state change, to tell whether a callback changed it.
    std::uint64_t transitions_ {0};
    // A queued timer completion can't be recalled: its handler checks this before using `this`.
    std::shared_ptr<bool> alive_ {std::make_shared<bool>(true)};
    net::steady_timer timer_;

    SubscribingSignal subscribingSignal_;
    SubscribedSignal subscribedSignal_;
    UnsubscribedSignal unsubscribedSignal_;
    PublicationSignal publicationSignal_;
    ErrorSignal errorSignal_;

    boost::signals2::connection onConnectingConnection_;
    boost::signals2::connection onConnectedConnection_;
    boost::signals2::connection onDisconnectedConnection_;
};

}
