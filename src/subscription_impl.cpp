#include "subscription_impl.h"

#include <algorithm>

#include <centrifugo/subscription.h>
#include <centrifugo/error.h>
#include "protocol_all.h"

namespace centrifugo {

namespace {

// A temporary subscribe or sub_refresh failure is retried after this long.
auto constexpr RETRY_DELAY = std::chrono::seconds {5};
// Centrifugo unsubscribe push codes from 2500 ask the client to subscribe again.
auto constexpr RESUBSCRIBE_CODE_MIN = 2500u;

}

SubscriptionImpl::SubscriptionImpl(std::string const &channel, Transport &transport,
                                   SubscriptionConfig config)
    : channel_ {channel}
    , transport_ {transport}
    , config_ {std::move(config)}
    , subscription_ {this}
    , token_ {config_.token}
    , timer_ {transport.executor()}
{
    init();
}

SubscriptionImpl::~SubscriptionImpl()
{
    deinit();
    failPendingHistory("subscription removed before the history reply");
}

auto SubscriptionImpl::state() const -> SubscriptionState
{
    return state_;
}

auto SubscriptionImpl::channel() const -> std::string const &
{
    return channel_;
}

auto SubscriptionImpl::subscription() -> Subscription &
{
    return subscription_;
}

auto SubscriptionImpl::subscribe() -> outcome::result<void, std::string>
{
    if (state_ != SubscriptionState::UNSUBSCRIBED) {
        return "cannot subscribe if not in unsubscribed state";
    }

    state_ = SubscriptionState::SUBSCRIBING;
    ++transitions_;
    tokenRetried_ = false;
    subscribingSignal_();

    if (state_ == SubscriptionState::SUBSCRIBING
        && transport_.state() == ConnectionState::Connected) {
        sendSubscribeCmd();
    }
    return outcome::success();
}

auto SubscriptionImpl::unsubscribe() -> void
{
    if (state_ == SubscriptionState::UNSUBSCRIBED) {
        return;
    }

    // Clear recovery state so a subsequent subscribe() starts fresh
    recoverable_ = false;
    epoch_.clear();
    offset_ = 0;

    // Like centrifuge-js: unsubscribed at once; replies to what was in flight are dropped.
    auto const sendUnsubscribe =
            serverMayHaveIt() && transport_.state() == ConnectionState::Connected;
    abandonPending();
    timer_.cancel();
    if (sendUnsubscribe) {
        sendCmd(makeCommand(UnsubscribeRequest {channel_}), CommandKind::Unsubscribe);
    }
    setState(SubscriptionState::UNSUBSCRIBED);
}

auto SubscriptionImpl::publish(nlohmann::json const &json) -> outcome::result<void, Error>
{
    if (state_ != SubscriptionState::SUBSCRIBED) {
        return Error {ErrorType::NotSubscribed, "not subscribed"};
    }
    sendCmd(makeCommand(PublishRequest {channel_, json}), CommandKind::Publish);
    return outcome::success();
}

auto SubscriptionImpl::history(HistoryOptions const &options,
                               Subscription::HistoryCallback onResult)
        -> outcome::result<void, Error>
{
    if (transport_.state() != ConnectionState::Connected) {
        return Error {ErrorType::NotConnected, "not connected"};
    }

    auto cmd = makeCommand(HistoryRequest {channel_, options.limit, options.since, options.reverse});
    pendingHistory_.emplace(cmd.id, std::move(onResult));
    transport_.send(std::move(cmd));
    return outcome::success();
}

auto SubscriptionImpl::handlePublish(Publication const &publication) -> void
{
    // Late publications after the subscription ended on our side.
    if (state_ == SubscriptionState::UNSUBSCRIBED) {
        return;
    }

    // Track stream position for recovery
    if (publication.offset > 0) {
        offset_ = publication.offset;
    }

    publicationSignal_(publication);
}

auto SubscriptionImpl::handleServerUnsubscribe(Unsubscribe const &unsubscribe) -> void
{
    if (state_ == SubscriptionState::UNSUBSCRIBED) {
        return;
    }
    timer_.cancel();
    abandonPending();

    if (unsubscribe.code < RESUBSCRIBE_CODE_MIN) {
        setState(SubscriptionState::UNSUBSCRIBED);
        return;
    }
    // E.g. the token expired: subscribe again, from the kept position. A stale token gets
    // 109 and is replaced from getToken.
    setState(SubscriptionState::SUBSCRIBING);
    if (state_ == SubscriptionState::SUBSCRIBING
        && transport_.state() == ConnectionState::Connected) {
        tokenRetried_ = false;
        sendSubscribeCmd();
    }
}

auto SubscriptionImpl::detach() -> void
{
    deinit();
    timer_.cancel();
    alive_.reset(); // a timer completion already queued must not act either
    // Not awaited (no longer routed here); lets the channel be subscribed again.
    if (serverMayHaveIt() && transport_.state() == ConnectionState::Connected) {
        transport_.send(makeCommand(UnsubscribeRequest {channel_}));
    }
    abandonPending();
}

auto SubscriptionImpl::serverMayHaveIt() const -> bool
{
    return state_ == SubscriptionState::SUBSCRIBED
        || std::any_of(waitingReplies_.begin(), waitingReplies_.end(),
                       [](auto const &w) { return w.second == CommandKind::Subscribe; });
}

auto SubscriptionImpl::onSubscribing() -> SubscribingSignal &
{
    return subscribingSignal_;
}

auto SubscriptionImpl::onSubscribed() -> SubscribedSignal &
{
    return subscribedSignal_;
}

auto SubscriptionImpl::onUnsubscribed() -> UnsubscribedSignal &
{
    return unsubscribedSignal_;
}

auto SubscriptionImpl::onPublication() -> PublicationSignal &
{
    return publicationSignal_;
}

auto SubscriptionImpl::onError() -> ErrorSignal &
{
    return errorSignal_;
}

auto SubscriptionImpl::init() -> void
{
    // A reply never crosses connections; the token is renewed by the resubscribe that follows.
    onConnectingConnection_ = transport_.onConnecting().connect([this](auto const &) {
        timer_.cancel();
        waitingReplies_.clear();
        if (state_ == SubscriptionState::SUBSCRIBED) {
            setState(SubscriptionState::SUBSCRIBING);
        }
        failPendingHistory("connection lost before the history reply");
    });

    onConnectedConnection_ = transport_.onConnected().connect([this](auto const &) {
        if (state_ == SubscriptionState::SUBSCRIBING) {
            tokenRetried_ = false;
            sendSubscribeCmd();
        }
    });

    onDisconnectedConnection_ = transport_.onDisconnected().connect([this](auto const &) {
        timer_.cancel();
        waitingReplies_.clear();
        failPendingHistory("disconnected before the history reply");
    });
}

auto SubscriptionImpl::deinit() -> void
{
    onConnectingConnection_.disconnect();
    onConnectedConnection_.disconnect();
    onDisconnectedConnection_.disconnect();
}

auto SubscriptionImpl::sendCmd(Command &&cmd, CommandKind kind) -> void
{
    waitingReplies_.emplace(cmd.id, kind);
    transport_.send(std::move(cmd));
}

auto SubscriptionImpl::abandonPending() -> void
{
    for (auto &[id, kind] : waitingReplies_) {
        if (kind == CommandKind::Subscribe || kind == CommandKind::SubRefresh) {
            kind = CommandKind::Abandoned;
        }
    }
}

auto SubscriptionImpl::usesToken() const -> bool
{
    return !config_.token.empty() || static_cast<bool>(config_.getToken);
}

auto SubscriptionImpl::fetchToken() -> outcome::result<std::string, Error>
{
    if (!config_.getToken) {
        return Error {ErrorType::Unauthorized, "subscription token needed but getToken is not set"};
    }
    auto const token = config_.getToken(channel_);
    if (!token) {
        return Error {ErrorType::Unauthorized,
                      "subscription getToken failed: " + token.error().message()};
    }
    return token.value();
}

auto SubscriptionImpl::sendSubscribeCmd() -> void
{
    auto req = SubscribeRequest {};
    req.channel = channel_;

    if (usesToken()) {
        if (token_.empty()) {
            auto token = fetchToken();
            if (!token) {
                endSubscription(token.error());
                return;
            }
            token_ = std::move(token).value();
        }
        req.token = token_;
    }

    // If we have a previous stream position, request recovery
    if (recoverable_ && !epoch_.empty()) {
        req.recover = true;
        req.epoch = epoch_;
        req.offset = offset_;
    }

    sendCmd(makeCommand(std::move(req)), CommandKind::Subscribe);
}

auto SubscriptionImpl::handleReply(Reply const &reply) -> bool
{
    if (auto const it = pendingHistory_.find(reply.id); it != pendingHistory_.end()) {
        auto onResult = std::move(it->second);
        pendingHistory_.erase(it);
        if (auto const *const err = std::get_if<ErrorReply>(&reply.result)) {
            onResult(Error {static_cast<ErrorType>(err->code), err->message});
        } else if (auto const *const result = std::get_if<HistoryResult>(&reply.result)) {
            onResult(*result);
        } else {
            onResult(Error {ErrorType::TransportError, "unexpected reply to history"});
        }
        return true;
    }

    auto const it = waitingReplies_.find(reply.id);
    if (it == waitingReplies_.end()) {
        return false;
    }
    auto const kind = it->second;
    waitingReplies_.erase(it);

    auto const *const error = std::get_if<ErrorReply>(&reply.result);
    switch (kind) {
    case CommandKind::Abandoned:
    case CommandKind::Unsubscribe:
        break;
    case CommandKind::Publish:
        if (error) {
            errorSignal_(Error {static_cast<ErrorType>(error->code), error->message});
        }
        break;
    case CommandKind::Subscribe:
        if (error) {
            handleSubscribeError(*error);
        } else if (auto const *const result = std::get_if<SubscribeResult>(&reply.result)) {
            handleSubscribed(*result);
        }
        break;
    case CommandKind::SubRefresh:
        if (error) {
            handleRefreshError(*error);
        } else if (auto const *const result = std::get_if<SubRefreshResult>(&reply.result);
                   result && state_ == SubscriptionState::SUBSCRIBED) {
            refreshRetried_ = false;
            if (result->expires) {
                startRefreshTimer(result->ttl);
            }
        }
        break;
    }
    return true;
}

auto SubscriptionImpl::handleSubscribed(SubscribeResult const &result) -> void
{
    if (state_ != SubscriptionState::SUBSCRIBING) {
        return;
    }
    tokenRetried_ = false;
    refreshRetried_ = false;

    // Store stream position for recovery on reconnect
    recoverable_ = result.recoverable;
    epoch_ = result.epoch;
    // When there are recovered publications, let handlePublish()
    // advance offset_ from each one. Otherwise use result.offset
    // as the baseline stream position.
    if (result.publications.empty()) {
        offset_ = result.offset;
    }
    if (result.expires) {
        startRefreshTimer(result.ttl);
    }

    setState(SubscriptionState::SUBSCRIBED);
    for (auto const &publication : result.publications) {
        handlePublish(publication);
    }
}

auto SubscriptionImpl::handleSubscribeError(ErrorReply const &error) -> void
{
    if (state_ != SubscriptionState::SUBSCRIBING) {
        return;
    }

    // Token expired: once per attempt, ask for a fresh one and subscribe again.
    if (static_cast<ErrorType>(error.code) == ErrorType::TokenExpired && usesToken()
        && !tokenRetried_) {
        tokenRetried_ = true;
        token_.clear();
        if (transport_.state() == ConnectionState::Connected) {
            sendSubscribeCmd();
        }
        return;
    }

    auto const err = Error {static_cast<ErrorType>(error.code), error.message};
    if (error.temporary) {
        // Stays SUBSCRIBING and tries again.
        startTimer(RETRY_DELAY, TimerAction::Resubscribe);
        errorSignal_(err);
        return;
    }
    endSubscription(err);
}

auto SubscriptionImpl::handleRefreshError(ErrorReply const &error) -> void
{
    if (state_ != SubscriptionState::SUBSCRIBED) {
        return;
    }
    // getToken may have returned a cached, already expired token: one fresh try.
    if (static_cast<ErrorType>(error.code) == ErrorType::TokenExpired && !refreshRetried_) {
        refreshRetried_ = true;
        token_.clear();
        refreshToken();
        return;
    }
    auto const err = Error {static_cast<ErrorType>(error.code), error.message};
    if (error.temporary) {
        startTimer(RETRY_DELAY, TimerAction::Refresh);
        errorSignal_(err);
        return;
    }
    endSubscription(err);
}

// Reports the error and ends the subscription, keeping its stream position so a later
// subscribe() can recover from it.
auto SubscriptionImpl::endSubscription(Error const &error) -> void
{
    timer_.cancel();
    auto const serverHasIt = state_ == SubscriptionState::SUBSCRIBED;
    abandonPending();
    if (serverHasIt && transport_.state() == ConnectionState::Connected) {
        sendCmd(makeCommand(UnsubscribeRequest {channel_}), CommandKind::Unsubscribe);
    }
    auto const before = transitions_;
    errorSignal_(error);
    // Unless the app changed the state from onError (e.g. unsubscribe() then subscribe()).
    if (transitions_ == before) {
        setState(SubscriptionState::UNSUBSCRIBED);
    }
}

auto SubscriptionImpl::startTimer(std::chrono::seconds const delay, TimerAction const action)
        -> void
{
    timer_.expires_after(delay);
    timer_.async_wait([this, alive = std::weak_ptr<bool> {alive_},
                       action](boost::system::error_code const ec) {
        // Checked first: a completion already queued runs even after cancel or destruction.
        if (ec || alive.expired()) {
            return;
        }
        if (action == TimerAction::Refresh) {
            refreshToken();
        } else if (state_ == SubscriptionState::SUBSCRIBING
                   && transport_.state() == ConnectionState::Connected) {
            sendSubscribeCmd();
        }
    });
}

auto SubscriptionImpl::startRefreshTimer(std::uint32_t const ttlSeconds) -> void
{
    if (ttlSeconds == 0) {
        return;
    }
    // At 3/4 of a short TTL, otherwise the configured lead before expiry.
    auto const ttl = std::chrono::seconds {ttlSeconds};
    startTimer(ttl - std::min(transport_.config().refreshTokenBeforeExpiry, ttl / 4),
               TimerAction::Refresh);
}

auto SubscriptionImpl::refreshToken() -> void
{
    if (state_ != SubscriptionState::SUBSCRIBED
        || transport_.state() != ConnectionState::Connected) {
        return;
    }
    if (!config_.getToken) {
        // A static token can't be renewed; the server unsubscribes us once it lapses.
        errorSignal_(Error {ErrorType::Unauthorized,
                            "subscription token expires but getToken is not set"});
        return;
    }

    auto token = fetchToken();
    if (!token) {
        endSubscription(token.error());
        return;
    }
    token_ = std::move(token).value();
    sendCmd(makeCommand(SubRefreshRequest {channel_, token_}), CommandKind::SubRefresh);
}

auto SubscriptionImpl::failPendingHistory(std::string const &reason) -> void
{
    // Swapped out first, since a callback may read history again.
    auto pending = std::unordered_map<std::uint32_t, Subscription::HistoryCallback> {};
    std::swap(pending, pendingHistory_);
    for (auto &[id, onResult] : pending) {
        onResult(Error {ErrorType::NotConnected, reason});
    }
}

auto SubscriptionImpl::setState(SubscriptionState newState) -> void
{
    if (state_ == newState)
        return;

    state_ = newState;
    ++transitions_;
    switch (state_) {
    case SubscriptionState::SUBSCRIBING:
        subscribingSignal_();
        break;
    case SubscriptionState::SUBSCRIBED:
        subscribedSignal_();
        break;
    case SubscriptionState::UNSUBSCRIBED:
        timer_.cancel();
        unsubscribedSignal_();
        break;
    }
}

}
