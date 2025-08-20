#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace centrifugo {

struct ClientConfig {
    std::string token;
    std::function<std::string()> getToken;
    std::string name;
    std::string version;

    std::chrono::seconds maxPingDelay {10};
    std::chrono::milliseconds minReconnectDelay {200};
    std::chrono::milliseconds maxReconnectDelay {20000};
};

enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED };

enum class DisconnectCode : std::uint16_t {
    NoError = 0,
    ConnectionError,

    Unauthorized,
    NoPing,

    Shutdown = 3001,
    BadRequest = 3501,
};

struct DisconnectReason {
    DisconnectCode code;
    std::string reason;
};

}
