#pragma once

#include <cstdint>
#include <string>
#include <functional>

#include <boost/outcome/result.hpp>
#include <nlohmann/json.hpp>

namespace centrifugo {

namespace outcome = boost::outcome_v2;

enum class LogLevel { Debug };

struct LogEntry {
    LogLevel level;
    std::string message;
    nlohmann::json fields;
};

struct ClientConfig {
    std::string token;
    std::function<outcome::result<std::string>()> getToken;
    std::string name;
    std::string version;

    std::chrono::seconds maxPingDelay {10};
    std::chrono::milliseconds minReconnectDelay {200};
    std::chrono::milliseconds maxReconnectDelay {20000};

    std::function<void(LogEntry)> logHandler;
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
