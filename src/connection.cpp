#include "connection.h"
#include "centrifugo/common.h"
#include "centrifugo/error.h"
#include "protocol_all.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace centrifugo {

auto parseUrl(std::string const &url) -> outcome::result<UrlComponents, std::string>
{
    auto parsedUrl = url;
    auto port = std::string {};
    auto secure = false;

    if (parsedUrl.substr(0, 6) == "wss://") {
        port = "443";
        secure = true;
        parsedUrl = parsedUrl.substr(6);
    } else if (parsedUrl.substr(0, 5) == "ws://") {
        port = "80";
        secure = false;
        parsedUrl = parsedUrl.substr(5);
    } else {
        return std::string {"URL must start with ws:// or wss://"};
    }

    auto colonPos = parsedUrl.find(':');
    auto slashPos = parsedUrl.find('/');

    std::string host;
    std::string path = "/";

    if (colonPos != std::string::npos && slashPos != std::string::npos) {
        // Both port and path specified: ws://host:port/path
        host = parsedUrl.substr(0, colonPos);
        port = parsedUrl.substr(colonPos + 1, slashPos - colonPos - 1);
        path = parsedUrl.substr(slashPos);
    } else if (colonPos != std::string::npos) {
        // Only port specified: ws://host:port
        host = parsedUrl.substr(0, colonPos);
        port = parsedUrl.substr(colonPos + 1);
    } else if (slashPos != std::string::npos) {
        // Only path specified: ws://host/path
        host = parsedUrl.substr(0, slashPos);
        path = parsedUrl.substr(slashPos);
    } else {
        // Only host specified: ws://host
        host = parsedUrl;
    }

    if (host.empty()) {
        return std::string {"Host cannot be empty"};
    }

    return UrlComponents {host, port, path, secure};
}

Connection::Connection(net::strand<net::io_context::executor_type> const &strand, std::string &&url,
                       ClientConfig &&config)
    : config_ {std::move(config)}
    , url_ {std::move(url)}
    , resolver_ {strand}
    , ws_ {WsStream {strand}}
    , reconnectTimer_ {strand}
    , pingTimer_ {strand}
    , tokenRefreshTimer_ {strand}
    , rng_ {std::random_device {}()}
{
    connectingSignal_.connect([this](auto const &) { reconnectAttempts_ = 0; });

    connectedSignal_.connect([this](ConnectResult const &result) {
        clientId_ = result.client;

        if (result.pong) {
            pingInterval_ = chrono::seconds {result.ping} + config_.maxPingDelay;
            startPingTimer();
        }

        if (result.expires) {
            startTokenRefreshTimer(result.ttl);
        }
    });

    disconnectedSignal_.connect([this](auto const &) {
        reconnectTimer_.cancel();
        pingTimer_.cancel();
        tokenRefreshTimer_.cancel();
        withWs([this](auto &ws) {
            ws.async_close(websocket::close_code::normal, [this](beast::error_code ec) {
                if (ec) {
                    errorSignal_("error while closing socket: " + ec.message());
                }
            });
        });
    });
}

auto Connection::state() const -> ConnectionState
{
    return state_;
}

auto Connection::sentCommands() const -> std::unordered_map<std::uint32_t, Command> const &
{
    return sentCommands_;
}

auto Connection::initialConnect() -> outcome::result<void, std::string>
{
    if (state_ != ConnectionState::DISCONNECTED) {
        return std::string {"already connected or connecting"};
    }
    if (config_.minReconnectDelay >= config_.maxReconnectDelay) {
        return std::string {"maxReconnectDelay should be greater than minReconnectDelay"};
    }
    if (config_.minReconnectDelay.count() > 0xFFFF) {
        return std::string {"minReconnectDelay can't be greater than 2^16"};
    }
    if (config_.name.length() > 16) {
        return std::string {"Name cannot be longer than 16 characters"};
    }
    if (config_.version.length() > 16) {
        return std::string {"Version cannot be longer than 16 characters"};
    }

    auto parseResult = parseUrl(url_);
    if (parseResult.has_error()) {
        return "Failed to parse URL: " + parseResult.error();
    }

    urlComponents_ = parseResult.value();

    if (urlComponents_.secure) {
        sslContext_.emplace(net::ssl::context::tlsv13_client);
        sslContext_->set_default_verify_paths();
        sslContext_->set_verify_mode(net::ssl::verify_peer);

        auto executor = withWs([](auto &ws) { return ws.get_executor(); });
        ws_.emplace<WssStream>(tcp::socket {executor}, *sslContext_);
    }

    connect();
    return outcome::success();
}

auto Connection::disconnect(DisconnectReason const &reason) -> void
{
    setState(ConnectionState::DISCONNECTED, reason);
}

auto Connection::send(json const &j, Command &&cmd) -> void
{
    if (pendingWrites_.empty()) {
        pendingWrites_ += j.dump();
    } else {
        pendingWrites_ += "\n" + j.dump();
    }

    if (cmd.id != 0) {
        pendingCommands_.push_back(std::move(cmd));
    }

    net::defer([this] { flush(); });
}

auto Connection::connect() -> void
{
    setState(ConnectionState::CONNECTING,
             DisconnectReason {DisconnectCode::NoError, "connect called"});

    if (token_.empty()) {
        token_ = !config_.token.empty() ? config_.token
               : config_.getToken       ? config_.getToken()
                                        : "";
    } else if (isTokenExpired_) {
        if (!refreshToken()) {
            return;
        }
        isTokenExpired_ = false;
    }

    resolver_.async_resolve(urlComponents_.host, urlComponents_.port,
                            [this](beast::error_code ec, tcp::resolver::results_type results) {
                                if (ec) {
                                    errorSignal_("resolve error: " + ec.message());
                                    reconnect();
                                    return;
                                }

                                withWs([this, results](auto &ws) {
                                    net::async_connect(
                                            beast::get_lowest_layer(ws), results,
                                            [this](beast::error_code ec,
                                                   tcp::resolver::results_type::endpoint_type) {
                                                if (ec) {
                                                    errorSignal_("connect error: " + ec.message());
                                                    reconnect();
                                                    return;
                                                }
                                                handShake();
                                            });
                                });
                            });
}

auto Connection::reconnect(DisconnectReason const &reason) -> void
{
    setState(ConnectionState::CONNECTING, reason);
    ++reconnectAttempts_;
    auto const delay = calculateBackoffDelay();
    std::cout << "reconnection attempt " << reconnectAttempts_ << " in " << delay.count() << "ms"
              << std::endl;

    reconnectTimer_.expires_after(delay);
    reconnectTimer_.async_wait([this](beast::error_code ec) {
        if (ec) {
            errorSignal_("reconnect timer error: " + ec.message());
            return;
        }

        connect();
    });
}

auto Connection::handShake() -> void
{
    withWs([this](auto &ws) {
        using StreamType = std::decay_t<decltype(ws)>;

        if constexpr (std::is_same_v<StreamType, WssStream>) {
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                          urlComponents_.host.c_str())) {
                auto ec = beast::error_code {static_cast<int>(::ERR_get_error()),
                                             net::error::get_ssl_category()};
                errorSignal_("failed to set SNI hostname: " + ec.message());
                reconnect();
                return;
            }

            ws.next_layer().async_handshake(
                    net::ssl::stream_base::client, [this](beast::error_code ec) {
                        if (ec) {
                            errorSignal_("SSL handshake error: " + ec.message());
                            reconnect();
                            return;
                        }

                        withWs([this](auto &ws) {
                            ws.async_handshake(urlComponents_.host, urlComponents_.path,
                                               [this](beast::error_code ec) {
                                                   if (ec) {
                                                       errorSignal_("webSocket handshake error: "
                                                                    + ec.message());
                                                       reconnect();
                                                       return;
                                                   }
                                                   sendConnectCmd();
                                                   read();
                                               });
                        });
                    });
        } else {
            ws.async_handshake(urlComponents_.host, urlComponents_.path,
                               [this](beast::error_code ec) {
                                   if (ec) {
                                       errorSignal_("webSocket handshake error: " + ec.message());
                                       reconnect();
                                       return;
                                   }
                                   sendConnectCmd();
                                   read();
                               });
        }
    });
}

auto Connection::read() -> void
{
    withWs([this](auto &ws) {
        ws.async_read(buffer_, [this, &ws](beast::error_code ec, std::size_t) {
            if (ec && ec != net::ssl::error::stream_truncated) {
                if (ec == beast::errc::operation_canceled) {
                    return;
                }

                if (ec != websocket::error::closed) {
                    errorSignal_(std::string {"read error: "} + ec.category().name() + ' '
                                 + ec.message());
                    reconnect(
                            DisconnectReason {DisconnectCode::ConnectionError, "connection error"});
                    return;
                }

                auto reason = DisconnectReason {static_cast<DisconnectCode>(ws.reason().code),
                                                std::string {ws.reason().reason}};
                if (reason.code == DisconnectCode::BadRequest) {
                    disconnect(reason);
                    return;
                }
                reconnect(reason);
            }

            auto data = beast::buffers_to_string(buffer_.data());
            buffer_.consume(buffer_.size());
            std::cout << "\033[34m┌── received:\033[0m\n"
                      << data << "\n\033[34m└──\033[0m" << std::endl;

            auto ss = std::stringstream {data};
            auto line = std::string {};
            while (std::getline(ss, line)) {
                if (line.empty()) {
                    continue;
                }

                try {
                    handleReceivedMsg(json::parse(line));
                } catch (std::exception const &e) {
                    errorSignal_("JSON parse error: " + std::string(e.what()));
                }
            }

            read();
        });
    });
}

auto Connection::handleReceivedMsg(json const &json) -> void
{
    if (json.empty()) {
        if (pingTimer_.cancel() == 0) // do not pong if not pinging
            return;

        startPingTimer();
        send(json::object());
        return;
    }

    auto const reply = json.get<Reply>();

    if (reply.error && static_cast<ErrorType>(reply.error->code) == ErrorType::TokenExpired) {
        isTokenExpired_ = true;
        reconnect();
    }

    if (reply.result) {
        std::visit(
                [this](auto const &result) {
                    using ResultType = std::decay_t<decltype(result)>;

                    if constexpr (std::is_same_v<ResultType, ConnectResult>) {
                        setState(ConnectionState::CONNECTED, result);
                    } else if constexpr (std::is_same_v<ResultType, RefreshResult>) {
                        if (result.expires) {
                            startTokenRefreshTimer(result.ttl);
                        }
                    }
                },
                *reply.result);
    }

    replyReceivedSignal_(reply);

    sentCommands_.erase(reply.id);
}

auto Connection::sendConnectCmd() -> void
{
    auto req = ConnectRequest {};
    req.token = token_;
    req.name = config_.name.empty() ? "cpp" : config_.name;
    req.version = config_.version;
    send(makeCommand(req));
}

auto Connection::flush() -> void
{
    if (isWriting_ || pendingWrites_.empty()) {
        return;
    }

    auto const messages = std::move(pendingWrites_);
    auto const commands = std::move(pendingCommands_);
    pendingWrites_.clear();
    pendingCommands_.clear();

    std::cout << "\033[31m┌── sending:\033[0m\n" << messages << "\n\033[31m└──\033[0m" << std::endl;

    isWriting_ = true;
    withWs([&](auto &ws) {
        ws.async_write(net::buffer(messages), [this, cmds = std::move(commands)](
                                                      beast::error_code ec, std::size_t) mutable {
            isWriting_ = false;

            if (ec) {
                errorSignal_("send error: " + ec.message());
                return;
            }

            for (auto &cmd : cmds) {
                sentCommands_.emplace(cmd.id, std::move(cmd));
            }

            if (!pendingWrites_.empty()) {
                flush();
            }
        });
    });
}

auto Connection::refreshToken() -> bool
{
    if (!config_.getToken) {
        errorSignal_("getToken must be set to handle token refresh");
        setState(ConnectionState::DISCONNECTED,
                 DisconnectReason {DisconnectCode::Unauthorized, "unauthorized"});
        return false;
    }
    token_ = config_.getToken();
    return true;
}

auto Connection::startPingTimer() -> void
{
    pingTimer_.expires_after(pingInterval_);
    pingTimer_.async_wait([this](boost::system::error_code ec) {
        if (ec) {
            if (ec == net::error::operation_aborted) {
                return;
            }
            errorSignal_("ping timer error: " + ec.message());
            return;
        }

        reconnect(DisconnectReason {DisconnectCode::NoPing, "no ping"});
    });
}

auto Connection::startTokenRefreshTimer(std::uint32_t ttlSeconds) -> void
{
    tokenRefreshTimer_.expires_after(chrono::seconds {ttlSeconds});
    tokenRefreshTimer_.async_wait([this](boost::system::error_code ec) {
        if (ec) {
            if (ec == net::error::operation_aborted) {
                return;
            }
            errorSignal_("token refresh timer error: " + ec.message());
            return;
        }

        if (!refreshToken()) {
            return;
        }

        send(makeCommand(RefreshRequest {token_}));
    });
}

auto Connection::calculateBackoffDelay() -> std::chrono::milliseconds
{
    using namespace std;
    using namespace chrono;

    auto constexpr BIT_SHIFT_BASE = 1u;
    auto constexpr MAX_EXPONENTIAL_SHIFT = static_cast<decltype(reconnectAttempts_)>(
            numeric_limits<decltype(BIT_SHIFT_BASE)>::digits / 2);

    auto const exponentialDelay =
            config_.minReconnectDelay
            * (BIT_SHIFT_BASE << min(reconnectAttempts_, MAX_EXPONENTIAL_SHIFT));
    auto const cappedDelay = min(exponentialDelay, config_.maxReconnectDelay);

    return milliseconds {
            uniform_int_distribution<milliseconds::rep> {0, cappedDelay.count()}(rng_)};
}

}
