#pragma once

#include <functional>
#include <string>

#include <boost/outcome/outcome.hpp>

#include <centrifugo/procotol.h>
#include <centrifugo/error.h>

namespace centrifugo {

namespace outcome = boost::outcome_v2;

enum class SubscriptionState { UNSUBSCRIBED, SUBSCRIBING, SUBSCRIBED };

// Subscription token for channels the server does not open to every client
// (`allow_subscribe_for_client: false`). Without token and getToken, no token is sent.
struct SubscriptionConfig {
    // Used for the first subscribe; getToken supplies later ones.
    std::string token;
    // Called on the strand when a token is needed: on subscribe without one, when the server
    // answers token expired (109), and ahead of the token's expiry to refresh it. An error ends
    // the subscription: onError, then UNSUBSCRIBED.
    std::function<outcome::result<std::string>(std::string const &channel)> getToken;
};

class SubscriptionImpl;

class Subscription
{
public:
    using HistoryCallback = std::function<void(outcome::result<HistoryResult, Error> const &)>;

    Subscription(SubscriptionImpl *impl);

    auto state() const -> SubscriptionState;
    auto channel() const -> std::string const &;

    auto subscribe() -> outcome::result<void, std::string>;
    auto unsubscribe() -> void;
    auto publish(nlohmann::json const &json) -> outcome::result<void, Error>;

    // Reads the channel's history; the server decides whether this client may. An error returned
    // here means nothing was sent and onResult never runs. Otherwise onResult runs exactly once,
    // later, on the strand: the history, the server's error, or NotConnected if the connection
    // drops first, or the subscription is removed. Exception: a read still pending when the
    // client is destroyed completes inside ~Client, on that thread; don't touch the client then.
    auto history(HistoryOptions const &options, HistoryCallback onResult)
            -> outcome::result<void, Error>;

    auto onSubscribing(std::function<void()> callback) -> void;
    auto onSubscribed(std::function<void()> callback) -> void;
    auto onUnsubscribed(std::function<void()> callback) -> void;
    auto onPublication(std::function<void(Publication const &)> callback) -> void;
    auto onError(std::function<void(Error const &)> callback) -> void;

private:
    SubscriptionImpl *impl = nullptr;
};

}
