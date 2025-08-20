#include "centrifugo/common.h"
#include <cstdint>
#include <functional>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <centrifugo.h>

auto getJwtToken() -> std::string
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    using tcp = net::ip::tcp;

    try {
        auto ioc = net::io_context {};
        auto stream = beast::tcp_stream {ioc};

        // Resolve and connect to JWT service
        auto const results = tcp::resolver {ioc}.resolve("localhost", "3001");
        stream.connect(results);

        // Set up HTTP GET request
        auto req = http::request<http::string_body> {http::verb::get,
                                                     "/token/server-side-user?seconds=300", 11};
        req.set(http::field::host, "localhost:3001");
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Send the HTTP request
        http::write(stream, req);

        // Read the HTTP response
        auto buffer = beast::flat_buffer {};
        auto res = http::response<http::string_body> {};
        http::read(stream, buffer, res);

        // Check response status
        if (res.result() != http::status::ok) {
            std::cerr << "HTTP error: " << res.result_int() << std::endl;
            return "";
        }

        // Extract token from response body
        auto const token = std::string {res.body()};

        // Gracefully close the socket
        auto ec = beast::error_code {};
        auto const _ = stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        return token;
    } catch (std::exception const &e) {
        std::cerr << "Error getting JWT token: " << e.what() << std::endl;
        return "";
    }
}

int main()
{
    auto ioc = boost::asio::io_context {};
    auto strand = boost::asio::make_strand(ioc);

    auto config = centrifugo::ClientConfig {};
    config.getToken = getJwtToken;
    auto client = centrifugo::Client {strand, "ws://localhost:8000/connection/websocket", config};

    client.onConnecting([](centrifugo::DisconnectReason const &reason) {
        std::cout << "[CLIENT] Connecting to Centrifugo... ("
                  << static_cast<std::uint16_t>(reason.code) << ", " << reason.reason << ')'
                  << std::endl;
    });

    client.onConnected([] { std::cout << "[CLIENT] Connected to Centrifugo!" << std::endl; });

    client.onDisconnected([](centrifugo::DisconnectReason const &reason) {
        std::cout << "[CLIENT] Disconnected from Centrifugo ("
                  << static_cast<std::uint16_t>(reason.code) << ", " << reason.reason << ')'
                  << std::endl;
    });

    client.onSubscribing([](std::string const &channel) {
        std::cout << "[SERVER-SUB:" << channel << "] Subscribing..." << std::endl;
    });

    client.onSubscribed([&client](std::string const &channel) {
        std::cout << "[SERVER-SUB:" << channel << "] Subscribed successfully!" << std::endl;

        auto pubRes = client.publish(channel, {{"message", "I am freeeeeee!!"}});
        if (!pubRes) {
            std::cout << "failed to publish: " << pubRes.error().message << std::endl;
        }
    });

    client.onUnsubscribed([](std::string const &channel) {
        std::cout << "[SERVER-SUB:" << channel << "] Unsubscribed" << std::endl;
    });

    client.onPublication([](std::string const &channel, centrifugo::Publication const &pub) {
        std::cout << "[SERVER-SUB:" << channel << "] Publication received:" << std::endl;
        std::cout << "  Data: " << pub.data << std::endl;
        std::cout << "  Offset: " << pub.offset << std::endl;
        if (pub.info) {
            std::cout << "  From user: " << pub.info->user << " (client: " << pub.info->client
                      << ")" << std::endl;
        }
    });

    auto subCreateRes = client.newSubscription("mychan");
    if (!subCreateRes) {
        std::cout << "failed creating subscription: " << subCreateRes.error() << std::endl;
        return 1;
    }
    auto &sub = subCreateRes.value().get();

    sub.onSubscribing([ch = sub.channel()] {
        std::cout << "Subscribing to channel '" << ch << "'..." << std::endl;
    });

    sub.onSubscribed([subRef = subCreateRes.value()] {
        auto &sub = subRef.get();
        std::cout << "Subscribed to channel '" << sub.channel() << "'!" << std::endl;

        auto pubRes = sub.publish({{"message", "I am freeeeeee!!"}});
        if (!pubRes) {
            std::cout << "failed to publish: " << pubRes.error().message << std::endl;
        }
    });

    sub.onUnsubscribed([ch = sub.channel()] {
        std::cout << "Unsubscribed from channel '" << ch << "'" << std::endl;
    });

    sub.onPublication([ch = sub.channel()](centrifugo::Publication const &pub) {
        std::cout << "Publication from channel '" << ch << "':\n" << pub.data << std::endl;
    });

    sub.onError([ch = sub.channel()](centrifugo::Error const &err) {
        std::cout << "Error in subscription '" << ch << "': " << err.message << std::endl;
    });

    if (auto subRes = sub.subscribe(); subRes.has_error()) {
        std::cout << "failed creating subscription: " << subCreateRes.error() << std::endl;
        return 1;
    }

    std::cout << "Starting Centrifugo client..." << std::endl;

    if (auto const conRes = client.connect(); !conRes) {
        std::cout << "Failed to connect: " << conRes.error() << std::endl;
        return 1;
    }

    ioc.run();

    return 0;
}
