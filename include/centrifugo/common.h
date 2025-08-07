#pragma once

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

}
