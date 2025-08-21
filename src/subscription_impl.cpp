#include "subscription_impl.h"

#include <centrifugo/subscription.h>
#include <centrifugo/error.h>
#include "protocol_all.h"

namespace centrifugo {

SubscriptionImpl::SubscriptionImpl(std::string const &channel, Transport &transport)
    : channel_ {channel}
    , transport_ {transport}
    , subscription_ {this}
{
    init();
}

SubscriptionImpl::~SubscriptionImpl()
{
    deinit();
}

SubscriptionImpl::SubscriptionImpl(SubscriptionImpl &&other) noexcept
    : channel_ {std::move(other.channel_)}
    , transport_ {other.transport_}
    , subscription_ {this}
    , state_ {other.state_}
    , subscribingSignal_ {std::move(other.subscribingSignal_)}
    , subscribedSignal_ {std::move(other.subscribedSignal_)}
    , unsubscribedSignal_ {std::move(other.unsubscribedSignal_)}
    , publicationSignal_ {std::move(other.publicationSignal_)}
{
    other.deinit();
    init();
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
    subscribingSignal_();

    if (transport_.state() == ConnectionState::CONNECTED) {
        sendSubscribeCmd();
    }
    return outcome::success();
}

auto SubscriptionImpl::unsubscribe() -> void
{
    if (state_ == SubscriptionState::UNSUBSCRIBED) {
        return;
    }

    // TODO: Send unsubscribe command to server
    state_ = SubscriptionState::UNSUBSCRIBED;
    unsubscribedSignal_();
}

auto SubscriptionImpl::publish(nlohmann::json const &json) -> outcome::result<void, Error>
{
    if (state_ != SubscriptionState::SUBSCRIBED) {
        return Error {ErrorType::NotSubscribed, "not subscribed"};
    }
    sendCmd(makeCommand(PublishRequest {channel_, json}));
    return outcome::success();
}

auto SubscriptionImpl::handlePublish(Publication const &publication) -> void
{
    publicationSignal_(publication);
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
    onConnectingConnection_ = transport_.onConnecting().connect([this](auto const &) {
        if (state_ == SubscriptionState::SUBSCRIBED) {
            setState(SubscriptionState::SUBSCRIBING);
        }
    });

    onConnectedConnection_ = transport_.onConnected().connect([this](auto const &) {
        if (state_ == SubscriptionState::SUBSCRIBING) {
            sendSubscribeCmd();
        }
    });
}

auto SubscriptionImpl::deinit() -> void
{
    onConnectingConnection_.disconnect();
    onConnectedConnection_.disconnect();
}

auto SubscriptionImpl::sendCmd(Command &&cmd) -> void
{
    waitingReplies_.emplace(cmd.id);
    transport_.send(std::move(cmd));
}

auto SubscriptionImpl::sendSubscribeCmd() -> void
{
    auto req = SubscribeRequest {};
    req.channel = channel_;
    sendCmd(makeCommand(std::move(req)));
}

auto SubscriptionImpl::handleReply(Reply const &reply) -> bool
{
    if (waitingReplies_.erase(reply.id) == 0) {
        return false;
    }
    std::visit(
            [this](auto const &result) {
                using ResultType = std::decay_t<decltype(result)>;

                if constexpr (std::is_same_v<ResultType, ErrorReply>) {
                    errorSignal_(Error {static_cast<ErrorType>(result.code), result.message});
                } else if constexpr (std::is_same_v<ResultType, SubscribeResult>) {
                    setState(SubscriptionState::SUBSCRIBED);
                }
            },
            reply.result);
    return true;
}

auto SubscriptionImpl::setState(SubscriptionState newState) -> void
{
    if (state_ == newState)
        return;

    state_ = newState;
    switch (state_) {
    case SubscriptionState::SUBSCRIBING:
        subscribingSignal_();
        break;
    case SubscriptionState::SUBSCRIBED:
        subscribedSignal_();
        break;
    case SubscriptionState::UNSUBSCRIBED:
        unsubscribedSignal_();
        break;
    }
}

}
