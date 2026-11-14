// Integration test: history recovery for server-side (JWT `channels` claim) subscriptions.
// Needs Centrifugo + jwt-generator from docker-compose.yml; run via tests/run_integration.sh,
// which exports CENTRIFUGO_PORT and JWT_PORT (defaults 8000 / 3001).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <centrifugo.h>
#include <centrifugo/common.h>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace outcome = boost::outcome_v2;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

auto constexpr API_KEY = "api-key";
auto constexpr WAIT_TIMEOUT = 10s;
// Quiet period after the expected publications arrive, to catch late duplicates.
auto constexpr SETTLE_TIME = 1500ms;
// Centrifugo: 3000-3499 and 4000-4499 reconnect; 3500-3999 and 4500-4999 are terminal.
auto constexpr RECONNECT_CODE = 3000;
auto constexpr TERMINAL_CODE = 4500;
auto constexpr TOKEN_EXPIRED = 109;

auto envPort(char const *name, char const *fallback) -> std::string
{
    auto const *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

auto const CENTRIFUGO_PORT = envPort("CENTRIFUGO_PORT", "8000");
auto const JWT_PORT = envPort("JWT_PORT", "3001");

auto httpRequest(http::verb verb, std::string const &port, std::string const &target,
                 std::string const &body = {}) -> std::optional<std::string>
{
    try {
        auto ioc = net::io_context {};
        auto stream = beast::tcp_stream {ioc};
        stream.connect(net::ip::tcp::resolver {ioc}.resolve("127.0.0.1", port));

        auto req = http::request<http::string_body> {verb, target, 11};
        req.set(http::field::host, "127.0.0.1:" + port);
        if (!body.empty()) {
            req.set(http::field::content_type, "application/json");
            req.set("X-API-Key", API_KEY);
            req.body() = body;
            req.prepare_payload();
        }
        http::write(stream, req);

        auto buffer = beast::flat_buffer {};
        auto res = http::response<http::string_body> {};
        http::read(stream, buffer, res);

        auto ec = beast::error_code {};
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok) {
            std::cerr << "HTTP " << res.result_int() << " for " << target << ": " << res.body()
                      << '\n';
            return std::nullopt;
        }
        return res.body();
    } catch (std::exception const &e) {
        std::cerr << "HTTP request to :" << port << target << " failed: " << e.what() << '\n';
        return std::nullopt;
    }
}

auto apiCall(std::string const &method, json const &params) -> bool
{
    auto const res = httpRequest(http::verb::post, CENTRIFUGO_PORT, "/api/" + method,
                                 params.dump());
    if (!res) {
        return false;
    }
    // The HTTP API reports failures in the body with a 200 status.
    auto const reply = json::parse(*res, nullptr, false);
    if (reply.is_discarded() || reply.contains("error")) {
        std::cerr << method << " failed: " << *res << '\n';
        return false;
    }
    return true;
}

auto publish(std::string const &channel, std::string const &seq) -> bool
{
    return apiCall("publish", {{"channel", channel}, {"data", {{"seq", seq}}}});
}

auto apiDisconnect(std::string const &user, int code) -> bool
{
    return apiCall("disconnect",
                   {{"user", user}, {"disconnect", {{"code", code}, {"reason", "test"}}}});
}

auto uniqueSuffix() -> std::string
{
    static auto rng = std::mt19937 {std::random_device {}()};
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count())
         + "_" + std::to_string(rng() % 100000);
}

auto fmt(std::vector<std::string> const &v) -> std::string
{
    auto out = std::string {"["};
    for (auto const &s : v) {
        out += (out.size() > 1 ? "," : "") + s;
    }
    return out + "]";
}

// Drives one Client on its own io_context thread and records what the app would see.
class Harness
{
public:
    // Token query for the n-th getToken call (1-based); runs on the client's strand.
    // A "channels=" in it overrides the harness channel (jwt-generator takes the first).
    using TokenParams = std::function<std::string(int call)>;

    explicit Harness(std::string channel, TokenParams tokenParams = {})
        : channel_ {std::move(channel)}
        , user_ {"rt-" + uniqueSuffix()}
        , tokenParams_ {std::move(tokenParams)}
        , strand_ {net::make_strand(ioc_)}
        , client_ {strand_, "ws://127.0.0.1:" + CENTRIFUGO_PORT + "/connection/websocket",
                   makeConfig()}
    {
        client_.onConnecting([this](centrifugo::Error const &) {
            auto hook = std::function<void()> {};
            update([this, &hook] { std::swap(hook, onReconnecting_); });
            if (hook) {
                hook();
            }
        });
        client_.onSubscribing([this](std::string const &) {
            auto disconnect = false;
            update([this, &disconnect] { std::swap(disconnect, disconnectOnSubscribing_); });
            if (disconnect) {
                client_.disconnect();
            }
        });
        client_.onUnsubscribed([this](std::string const &) {
            auto disconnect = false;
            update([this, &disconnect] {
                ++unsubscribedCount_;
                std::swap(disconnect, disconnectOnUnsubscribed_);
            });
            if (disconnect) {
                client_.disconnect();
            }
        });
        client_.onConnected([this] {
            update([this] {
                connected_ = true;
                ++connectedCount_;
            });
        });
        client_.onDisconnected([this](centrifugo::Error const &) {
            update([this] {
                connected_ = false;
                ++disconnectedCount_;
            });
        });
        client_.onSubscribed([this](std::string const &ch) {
            update([this, ch] {
                if (ch == channel_) {
                    ++subscribedCount_;
                }
            });
        });
        client_.onPublication([this](std::string const &ch, centrifugo::Publication const &pub) {
            update([this, ch, pub] {
                if (ch == channel_) {
                    received_.push_back(pub.data.value("seq", std::string {"?"}));
                    offsets_.push_back(pub.offset);
                }
            });
        });
        client_.onError([this](centrifugo::Error const &e) {
            update([this, e] { errorCodes_.push_back(e.ec.value()); });
        });
        thread_ = std::thread {[this] { ioc_.run(); }};
    }

    ~Harness()
    {
        disconnect();
        {
            auto lock = std::unique_lock {mutex_};
            cv_.wait_for(lock, 2s, [this] { return !connected_; });
        }
        work_.reset();
        ioc_.stop();
        thread_.join();
    }

    Harness(Harness const &) = delete;
    auto operator=(Harness const &) -> Harness & = delete;

    auto user() const -> std::string const & { return user_; }

    auto connect() -> void
    {
        net::post(strand_, [this] {
            if (auto const res = client_.connect(); !res) {
                std::cerr << "  connect() failed: " << res.error().message << '\n';
            }
        });
    }

    auto disconnect() -> void
    {
        net::post(strand_, [this] { client_.disconnect(); });
    }

    // Runs once, on the strand, when the client next starts (re)connecting.
    auto onNextReconnect(std::function<void()> hook) -> void
    {
        update([this, &hook] { onReconnecting_ = std::move(hook); });
    }

    // The app calls Client::disconnect() from the next onSubscribing callback.
    auto disconnectOnNextSubscribing() -> void
    {
        update([this] { disconnectOnSubscribing_ = true; });
    }

    // The app calls Client::disconnect() from the next onUnsubscribed callback.
    auto disconnectOnNextUnsubscribed() -> void
    {
        update([this] { disconnectOnUnsubscribed_ = true; });
    }

    // Client-level publish on the strand. Records the code publish() returned (0: sent) and each
    // result callback's code (0: accepted); disconnectAfter calls disconnect() right after.
    auto clientPublish(std::string const &channel, json const &data, bool disconnectAfter = false)
            -> void
    {
        net::post(strand_, [this, channel, data, disconnectAfter] {
            publishRecorded(channel, data);
            if (disconnectAfter) {
                client_.disconnect();
            }
        });
    }

    // Publishes big, then, while its write is still in flight, small, then disconnects: small is
    // left queued behind an interrupted write.
    auto publishDuringWriteThenDisconnect(std::string const &channel, json const &big,
                                          json const &small) -> void
    {
        net::post(strand_, [this, channel, big, small] {
            publishRecorded(channel, big);
            // Queued behind the flush that publish posted, so the big write has started.
            net::post(strand_, [this, channel, small] {
                publishRecorded(channel, small);
                client_.disconnect();
            });
        });
    }

    auto waitFor(std::string const &what, std::function<bool()> const &pred) -> bool
    {
        auto lock = std::unique_lock {mutex_};
        if (cv_.wait_for(lock, WAIT_TIMEOUT, pred)) {
            return true;
        }
        std::cerr << "  FAIL: timed out after " << WAIT_TIMEOUT.count() << "s waiting for "
                  << what << " (received: " << fmt(received_) << ")\n";
        return false;
    }

    template<typename T>
    auto snapshot(T Harness::*member) -> T
    {
        auto lock = std::lock_guard {mutex_};
        return this->*member;
    }

    auto received() -> std::vector<std::string> { return snapshot(&Harness::received_); }
    auto offsets() -> std::vector<std::uint64_t> { return snapshot(&Harness::offsets_); }
    auto errorCodes() -> std::vector<int> { return snapshot(&Harness::errorCodes_); }
    auto notRecoveredLogs() -> int { return snapshot(&Harness::notRecoveredLogs_); }
    auto connectSubs() -> std::vector<json> { return snapshot(&Harness::connectSubs_); }
    auto tokenCalls() -> int { return snapshot(&Harness::tokenCalls_); }
    auto subscribed() -> int { return snapshot(&Harness::subscribedCount_); }
    auto unsubscribed() -> int { return snapshot(&Harness::unsubscribedCount_); }
    auto publishSendCodes() -> std::vector<int> { return snapshot(&Harness::publishSendCodes_); }
    auto publishResults() -> std::vector<int> { return snapshot(&Harness::publishResults_); }

    // Only for waitFor predicates, which run with the lock held.
    auto receivedCount() const -> std::size_t { return received_.size(); }
    auto publishResultCount() const -> std::size_t { return publishResults_.size(); }
    auto publishSendCount() const -> std::size_t { return publishSendCodes_.size(); }
    auto subscribedCount() const -> int { return subscribedCount_; }
    auto disconnectedCount() const -> int { return disconnectedCount_; }
    auto connectedCount() const -> int { return connectedCount_; }
    auto connected() const -> bool { return connected_; }

private:
    auto publishRecorded(std::string const &channel, json const &data) -> void
    {
        auto const res = client_.publish(channel, data, [this](auto const &r) {
            update([this, code = r ? 0 : r.error().ec.value()] {
                publishResults_.push_back(code);
            });
        });
        update([this, code = res ? 0 : res.error().ec.value()] {
            publishSendCodes_.push_back(code);
        });
    }

    auto makeConfig() -> centrifugo::ClientConfig
    {
        auto config = centrifugo::ClientConfig {};
        config.name = "recovery-test";
        config.getToken = [this]() -> outcome::result<std::string> {
            auto call = 0;
            update([this, &call] { call = ++tokenCalls_; });
            auto const params = tokenParams_ ? tokenParams_(call) : std::string {"seconds=300"};
            auto const token = httpRequest(http::verb::get, JWT_PORT,
                                           "/token/" + user_ + "?" + params
                                                   + "&channels=" + channel_);
            if (!token) {
                return std::make_error_code(std::errc::connection_refused);
            }
            return *token;
        };
        config.logHandler = [this](centrifugo::LogEntry const &entry) {
            if (entry.level == centrifugo::LogLevel::Error) {
                std::cerr << "  log: " << entry.message << " " << entry.fields.dump() << '\n';
                if (entry.message.find("not recovered") != std::string::npos) {
                    update([this] { ++notRecoveredLogs_; });
                }
            } else if (entry.message == "received message") {
                recordConnectReply(entry.fields.value("message", std::string {}));
            }
        };
        return config;
    }

    // Keeps this channel's entry of every connect reply, to inspect recovery flags.
    auto recordConnectReply(std::string const &message) -> void
    {
        auto lines = std::istringstream {message};
        for (auto line = std::string {}; std::getline(lines, line);) {
            auto const j = json::parse(line, nullptr, false);
            if (j.is_object() && j.contains("connect")) {
                auto sub = j["connect"].value("subs", json::object()).value(channel_, json {});
                update([this, &sub] { connectSubs_.push_back(std::move(sub)); });
            }
        }
    }

    template<typename F>
    auto update(F &&f) -> void
    {
        {
            auto lock = std::lock_guard {mutex_};
            f();
        }
        cv_.notify_all();
    }

    std::string channel_;
    std::string user_;
    TokenParams tokenParams_;
    net::io_context ioc_;
    net::executor_work_guard<net::io_context::executor_type> work_ {ioc_.get_executor()};
    net::strand<net::io_context::executor_type> strand_;
    centrifugo::Client client_;
    std::thread thread_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()> onReconnecting_;
    bool disconnectOnSubscribing_ {false};
    bool disconnectOnUnsubscribed_ {false};
    bool connected_ {false};
    int subscribedCount_ {0};
    int unsubscribedCount_ {0};
    int disconnectedCount_ {0};
    int connectedCount_ {0};
    int notRecoveredLogs_ {0};
    int tokenCalls_ {0};
    std::vector<std::string> received_;
    std::vector<std::uint64_t> offsets_;
    std::vector<int> errorCodes_;
    std::vector<json> connectSubs_;
    std::vector<int> publishSendCodes_;
    std::vector<int> publishResults_;
};

auto expectEq(std::vector<std::string> const &actual, std::vector<std::string> const &expected,
              std::string const &what) -> bool
{
    if (actual == expected) {
        return true;
    }
    std::cerr << "  FAIL: " << what << ": expected " << fmt(expected) << ", got " << fmt(actual)
              << '\n';
    return false;
}

auto expectClean(Harness &h) -> bool
{
    auto ok = true;
    auto const offsets = h.offsets();
    for (auto i = std::size_t {1}; i < offsets.size(); ++i) {
        if (offsets[i] <= offsets[i - 1]) {
            std::cerr << "  FAIL: offsets not increasing at index " << i << '\n';
            ok = false;
        }
    }
    if (auto const n = h.notRecoveredLogs(); n != 0) {
        std::cerr << "  FAIL: client logged " << n << " 'not recovered' error(s)\n";
        ok = false;
    }
    return ok;
}

auto settle() -> void
{
    std::this_thread::sleep_for(SETTLE_TIME);
}

auto connectAndSettle(Harness &h, int subscribedBefore, std::size_t expectedCount) -> bool
{
    h.connect();
    if (!h.waitFor("subscribe #" + std::to_string(subscribedBefore + 1),
                   [&] { return h.subscribedCount() > subscribedBefore; })
        || !h.waitFor(std::to_string(expectedCount) + " publication(s)",
                      [&] { return h.receivedCount() >= expectedCount; })) {
        return false;
    }
    settle();
    return true;
}

auto disconnectAndWait(Harness &h) -> bool
{
    h.disconnect();
    return h.waitFor("disconnect", [&] { return !h.connected(); });
}

auto connectAndReceiveP1(Harness &h, std::string const &channel) -> bool
{
    h.connect();
    return h.waitFor("first subscribe", [&] { return h.subscribedCount() >= 1; })
        && publish(channel, "P1") && h.waitFor("P1", [&] { return h.receivedCount() >= 1; });
}

// Client::disconnect() then connect(): recovery from the tracked position.
auto testStreamClientReconnect() -> bool
{
    auto const channel = "stream:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndReceiveP1(h, channel) || !disconnectAndWait(h) || !publish(channel, "P2")
        || !publish(channel, "P3") || !connectAndSettle(h, 1, 3)) {
        return false;
    }
    return expectEq(h.received(), {"P1", "P2", "P3"}, "stream recovery") && expectClean(h);
}

// Server-forced disconnect with a reconnect code: the transport reconnects on its own.
auto testStreamTransportReconnectWith(int const code) -> bool
{
    auto const channel = "stream:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndReceiveP1(h, channel)) {
        return false;
    }
    // Publishing inside onConnecting guarantees the client is offline when P2/P3 land.
    h.onNextReconnect([&] { publish(channel, "P2") && publish(channel, "P3"); });
    if (!apiDisconnect(h.user(), code)
        || !h.waitFor("automatic re-subscribe", [&] { return h.subscribedCount() >= 2; })
        || !h.waitFor("P2, P3", [&] { return h.receivedCount() >= 3; })) {
        return false;
    }
    settle();
    return expectEq(h.received(), {"P1", "P2", "P3"}, "transport reconnect recovery")
        && expectClean(h);
}

auto testStreamTransportReconnect() -> bool
{
    return testStreamTransportReconnectWith(RECONNECT_CODE);
}

// 4000-4499 are application reconnect codes: the client must reconnect and recover.
auto testStreamApplicationReconnect() -> bool
{
    return testStreamTransportReconnectWith(4000);
}

// Connect token expires and its refresh is rejected (109): reconnect with a fresh token.
auto testStreamTokenExpired() -> bool
{
    auto const channel = "stream:" + uniqueSuffix();
    auto published = false;
    auto h = Harness {channel, [&](int call) -> std::string {
                          if (call == 1) {
                              return "seconds=3";
                          }
                          if (call == 2) {
                              return "seconds=-60"; // already expired
                          }
                          // Client is offline here; publish before it can reconnect.
                          if (!published) {
                              published = publish(channel, "P2") && publish(channel, "P3");
                          }
                          return "seconds=300";
                      }};
    if (!connectAndReceiveP1(h, channel)
        || !h.waitFor("re-subscribe after token expiry",
                      [&] { return h.subscribedCount() >= 2; })
        || !h.waitFor("P2, P3", [&] { return h.receivedCount() >= 3; })) {
        return false;
    }
    settle();
    auto ok = expectEq(h.received(), {"P1", "P2", "P3"}, "token-expired recovery")
           && expectClean(h);
    auto const codes = h.errorCodes();
    if (std::find(codes.begin(), codes.end(), TOKEN_EXPIRED) == codes.end()) {
        std::cerr << "  FAIL: no TokenExpired (109) error seen; token calls " << h.tokenCalls()
                  << '\n';
        ok = false;
    }
    return ok;
}

// More publications while offline than history_size (10): the client must say so.
auto testStreamHistoryOverflow() -> bool
{
    auto const channel = "stream:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndReceiveP1(h, channel) || !disconnectAndWait(h)) {
        return false;
    }
    for (auto i = 2; i <= 13; ++i) {
        if (!publish(channel, "P" + std::to_string(i))) {
            return false;
        }
    }
    if (!connectAndSettle(h, 1, 1)) {
        return false;
    }
    std::cout << "  received after failed recovery: " << fmt(h.received()) << '\n';
    if (auto const n = h.notRecoveredLogs(); n != 1) {
        std::cerr << "  FAIL: expected 1 'not recovered' log, got " << n << '\n';
        return false;
    }
    return true;
}

// Terminal server disconnect forgets positions: a later connect() starts fresh.
auto testStreamTerminalDisconnect() -> bool
{
    auto const channel = "stream:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndReceiveP1(h, channel) || !apiDisconnect(h.user(), TERMINAL_CODE)
        || !h.waitFor("terminal disconnect", [&] { return h.disconnectedCount() >= 1; })
        || !publish(channel, "P2") || !connectAndSettle(h, 1, 1) || !publish(channel, "P3")
        || !h.waitFor("P3", [&] { return h.receivedCount() >= 2; })) {
        return false;
    }
    settle();
    return expectEq(h.received(), {"P1", "P3"}, "no recovery after terminal disconnect")
        && expectClean(h);
}

// Cache mode where the client's recover request drives delivery.
auto testCacheClientRecover() -> bool
{
    auto const channel = "cachec:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndSettle(h, 0, 0) || !expectEq(h.received(), {}, "first connect")
        || !publish(channel, "X") || !h.waitFor("X live", [&] { return h.receivedCount() >= 1; })
        || !disconnectAndWait(h) || !publish(channel, "Y") || !connectAndSettle(h, 1, 2)
        || !expectEq(h.received(), {"X", "Y"}, "reconnect after publish")
        || !disconnectAndWait(h) || !connectAndSettle(h, 2, 2)) {
        return false;
    }
    return expectEq(h.received(), {"X", "Y"}, "no-change reconnect") && expectClean(h);
}

auto testCacheAutoRecoverFirstConnect() -> bool
{
    auto const channel = "cache:" + uniqueSuffix();
    if (!publish(channel, "X")) {
        return false;
    }
    auto h = Harness {channel};
    if (!connectAndSettle(h, 0, 1) || !expectEq(h.received(), {"X"}, "first connect")
        || !disconnectAndWait(h) || !publish(channel, "Y") || !connectAndSettle(h, 1, 2)
        || !expectEq(h.received(), {"X", "Y"}, "reconnect after publish")
        || !disconnectAndWait(h) || !connectAndSettle(h, 2, 2)) {
        return false;
    }
    return expectEq(h.received(), {"X", "Y"}, "no-change reconnect") && expectClean(h);
}

// Nothing ever published: the connect replies must not trigger a 'not recovered' log.
auto testCacheAutoRecoverEmpty() -> bool
{
    auto const channel = "cache:" + uniqueSuffix();
    auto h = Harness {channel};
    if (!connectAndSettle(h, 0, 0) || !disconnectAndWait(h) || !connectAndSettle(h, 1, 0)) {
        return false;
    }
    for (auto const &sub : h.connectSubs()) {
        std::cout << "  connect reply subs[" << channel << "]: " << sub.dump() << '\n';
    }
    return expectEq(h.received(), {}, "empty channel") && expectClean(h);
}

// Client::disconnect() from onSubscribing: no onSubscribed or publications may follow.
auto testDisconnectFromSubscribing() -> bool
{
    auto const channel = "cache:" + uniqueSuffix();
    if (!publish(channel, "X")) {
        return false;
    }
    auto h = Harness {channel};
    h.disconnectOnNextSubscribing();
    h.connect();
    if (!h.waitFor("disconnect", [&] { return h.disconnectedCount() >= 1; })) {
        return false;
    }
    settle();
    auto const subscribed = h.subscribed();
    if (subscribed != 0) {
        std::cerr << "  FAIL: onSubscribed fired " << subscribed << " time(s) after disconnect\n";
        return false;
    }
    return expectEq(h.received(), {}, "publications after disconnect");
}

// Reconnect drops two server subs; the app disconnects from the first onUnsubscribed.
// onDisconnected unsubscribes the other, so the drop loop must not do it again.
auto testDisconnectFromUnsubscribed() -> bool
{
    auto const suffix = uniqueSuffix();
    auto const a = "stream:a" + suffix;
    auto const b = "stream:b" + suffix;
    auto const c = "stream:c" + suffix;
    // Expire the token and reject its refresh (109): the reconnect then carries only c.
    auto h = Harness {a, [&](int call) -> std::string {
                          switch (call) {
                          case 1:
                              return "seconds=3&channels=" + a + "," + b;
                          case 2:
                              return std::string {"seconds=-60"};
                          default:
                              return "seconds=300&channels=" + c;
                          }
                      }};
    h.disconnectOnNextUnsubscribed();
    h.connect();
    if (!h.waitFor("first subscribe", [&] { return h.subscribedCount() >= 1; })
        || !h.waitFor("disconnect from onUnsubscribed",
                      [&] { return h.disconnectedCount() >= 1; })) {
        return false;
    }
    settle();
    if (auto const n = h.unsubscribed(); n != 2) {
        std::cerr << "  FAIL: expected 2 onUnsubscribed (a, b once each), got " << n << '\n';
        return false;
    }
    return true;
}

auto expectCodes(std::vector<int> const &actual, std::vector<int> const &expected,
                 std::string const &what) -> bool
{
    auto toStrings = [](std::vector<int> const &v) {
        auto out = std::vector<std::string> {};
        for (auto const i : v) {
            out.push_back(std::to_string(i));
        }
        return out;
    };
    return expectEq(toStrings(actual), toStrings(expected), what);
}

auto lastHistorySeq(std::string const &channel) -> std::string
{
    auto const res =
            httpRequest(http::verb::post, CENTRIFUGO_PORT, "/api/history",
                        json {{"channel", channel}, {"limit", 1}, {"reverse", true}}.dump());
    auto const reply = res ? json::parse(*res, nullptr, false) : json {};
    auto const pubs =
            reply.is_object()
                    ? reply.value("result", json::object()).value("publications", json::array())
                    : json::array();
    return pubs.empty() ? std::string {} : pubs[0].value("data", json::object()).value("seq", "");
}

// Publishes from a connected client to a channel it isn't subscribed to; expects one result.
auto runClientPublish(std::string const &channel, std::vector<int> const &expected) -> bool
{
    auto h = Harness {"stream:" + uniqueSuffix()};
    h.connect();
    if (!h.waitFor("connect", [&] { return h.connected(); })) {
        return false;
    }
    h.clientPublish(channel, {{"seq", "P"}});
    if (!h.waitFor("publish result", [&] { return h.publishResultCount() >= 1; })) {
        return false;
    }
    settle();
    return expectCodes(h.publishSendCodes(), {0}, "publish() return")
        && expectCodes(h.publishResults(), expected, "publish results (exactly once)");
}

auto testPublishAccepted() -> bool
{
    auto const channel = "pub:" + uniqueSuffix();
    if (!runClientPublish(channel, {0})) {
        return false;
    }
    return expectEq({lastHistorySeq(channel)}, {"P"}, "published data in history");
}

// No publish permission in the namespace: Centrifugo refuses with 103.
auto testPublishRefused() -> bool
{
    return runClientPublish("stream:" + uniqueSuffix(), {103});
}

// A failing publish proxy reaches the client as code 100 (temporary); it is not retried.
auto testPublishProxyFailure() -> bool
{
    return runClientPublish("proxied:" + uniqueSuffix(), {100});
}

auto testPublishNotConnected() -> bool
{
    auto h = Harness {"stream:" + uniqueSuffix()};
    h.clientPublish("pub:" + uniqueSuffix(), {{"seq", "P"}});
    if (!h.waitFor("publish() return", [&] { return h.publishSendCount() >= 1; })) {
        return false;
    }
    settle();
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(h.publishSendCodes(), {notConnected}, "publish() return")
        && expectCodes(h.publishResults(), {}, "no result callback");
}

// Disconnect before the publish goes out: the callback fails once and the publish is dropped,
// not sent by a later connection.
auto testPublishDisconnectBeforeSend() -> bool
{
    auto const channel = "pub:" + uniqueSuffix();
    auto h = Harness {"stream:" + uniqueSuffix()};
    h.connect();
    if (!h.waitFor("connect", [&] { return h.connected(); })) {
        return false;
    }
    h.clientPublish(channel, {{"seq", "P"}}, true);
    if (!h.waitFor("publish result", [&] { return h.publishResultCount() >= 1; })) {
        return false;
    }
    h.connect();
    if (!h.waitFor("reconnect", [&] { return h.connectedCount() >= 2; })) {
        return false;
    }
    settle();
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(h.publishSendCodes(), {0}, "publish() return")
        && expectCodes(h.publishResults(), {notConnected}, "publish results (exactly once)")
        && expectEq({lastHistorySeq(channel)}, {""}, "nothing published");
}

// A publish still queued when the connection drops is dropped with it: the next connection
// must start with connect, not send a publish already reported as failed.
auto testPublishQueuedAcrossDisconnect() -> bool
{
    auto const channel = "pub:" + uniqueSuffix();
    auto h = Harness {"stream:" + uniqueSuffix()};
    h.connect();
    if (!h.waitFor("connect", [&] { return h.connected(); })) {
        return false;
    }
    h.publishDuringWriteThenDisconnect(
            channel, {{"seq", "BIG"}, {"pad", std::string(8 << 20, 'x')}}, {{"seq", "S"}});
    if (!h.waitFor("publish results", [&] { return h.publishResultCount() >= 2; })) {
        return false;
    }
    h.connect();
    if (!h.waitFor("reconnect", [&] { return h.connectedCount() >= 2; })) {
        return false;
    }
    settle();
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(h.publishResults(), {notConnected, notConnected}, "publish results")
        && expectEq({lastHistorySeq(channel)}, {""}, "nothing published")
        && h.waitFor("still connected", [&] { return h.connected(); });
}

// The connection drops while the server holds the reply (slow proxy): the callback fails once,
// the client reconnects, and the old connection's reply never runs it again.
auto testPublishReconnectWhileWaiting() -> bool
{
    auto h = Harness {"stream:" + uniqueSuffix()};
    h.connect();
    if (!h.waitFor("connect", [&] { return h.connected(); })) {
        return false;
    }
    h.clientPublish("proxied:slow" + uniqueSuffix(), {{"seq", "P"}});
    if (!h.waitFor("publish sent", [&] { return h.publishSendCount() >= 1; })) {
        return false;
    }
    std::this_thread::sleep_for(500ms); // the proxy now holds the reply for 3 s
    if (!apiDisconnect(h.user(), RECONNECT_CODE)
        || !h.waitFor("publish result", [&] { return h.publishResultCount() >= 1; })
        || !h.waitFor("reconnect", [&] { return h.connectedCount() >= 2; })) {
        return false;
    }
    std::this_thread::sleep_for(4s); // past the stalled reply
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(h.publishSendCodes(), {0}, "publish() return")
        && expectCodes(h.publishResults(), {notConnected}, "publish results (exactly once)");
}

// Destroying the client with a publish in flight runs its callback once, with a failure.
auto testPublishClientDestroyed() -> bool
{
    auto ioc = net::io_context {};
    auto strand = net::make_strand(ioc);
    auto config = centrifugo::ClientConfig {};
    config.getToken = []() -> outcome::result<std::string> {
        auto const token = httpRequest(http::verb::get, JWT_PORT,
                                       "/token/rt-" + uniqueSuffix() + "?seconds=300");
        if (!token) {
            return std::make_error_code(std::errc::connection_refused);
        }
        return *token;
    };
    auto client = std::make_unique<centrifugo::Client>(
            strand, "ws://127.0.0.1:" + CENTRIFUGO_PORT + "/connection/websocket", config);
    auto results = std::vector<int> {};
    client->onConnected([&] {
        // Same handler: the reply cannot arrive before the client is gone.
        auto const res =
                client->publish("pub:" + uniqueSuffix(), {{"seq", "P"}}, [&](auto const &r) {
                    results.push_back(r ? 0 : r.error().ec.value());
                });
        if (!res) {
            results.push_back(-1);
        }
        // Stopped with it: the transport's outstanding handlers must never run on a dead client.
        net::post(strand, [&] {
            client.reset();
            ioc.stop();
        });
    });
    net::post(strand, [&] {
        if (!client->connect()) {
            results.push_back(-2);
        }
    });
    ioc.run_for(WAIT_TIMEOUT);
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectEq({client ? "alive" : "destroyed"}, {"destroyed"}, "client")
        && expectCodes(results, {notConnected}, "publish results (exactly once)");
}

// ==== Client-side subscriptions with subscription tokens (namespace "live", closed) ====

auto subToken(std::string const &user, std::string const &channel, int seconds) -> std::string
{
    return httpRequest(http::verb::get, JWT_PORT,
                       "/sub-token/" + user + "?channel=" + channel + "&seconds="
                               + std::to_string(seconds))
            .value_or("");
}

// One Client and one client-side subscription on its own io_context thread.
class SubHarness
{
public:
    // Query for the n-th subscription getToken call (1-based); empty: getToken fails.
    using SubTokenParams = std::function<std::string(int call)>;

    struct Options {
        bool withToken {true};                    // configure a SubscriptionConfig at all
        std::optional<int> initialTokenSeconds;   // SubscriptionConfig::token, signed for user
        SubTokenParams subTokenParams;            // null: no getToken
    };

    SubHarness(std::string channel, Options options)
        : channel_ {std::move(channel)}
        , user_ {"st-" + uniqueSuffix()}
        , options_ {std::move(options)}
        , strand_ {net::make_strand(ioc_)}
        , client_ {strand_, "ws://127.0.0.1:" + CENTRIFUGO_PORT + "/connection/websocket",
                   makeConfig()}
    {
        client_.onConnected([this] { update([this] { ++connectedCount_; }); });
        client_.onDisconnected([this](centrifugo::Error const &) {
            update([this] { connected_ = false; });
        });
        client_.onConnected([this] { update([this] { connected_ = true; }); });

        auto sub = options_.withToken ? client_.newSubscription(channel_, makeSubConfig())
                                      : client_.newSubscription(channel_);
        sub_ = &sub.value().get();
        sub_->onSubscribed([this] { update([this] { ++subscribed_; }); });
        sub_->onUnsubscribed([this] { update([this] { ++unsubscribed_; }); });
        sub_->onError([this](centrifugo::Error const &e) {
            update([this, e] { errors_.push_back(e.ec.value()); });
        });
        sub_->onPublication([this](centrifugo::Publication const &pub) {
            update([this, pub] { received_.push_back(pub.data.value("seq", std::string {"?"})); });
        });
        thread_ = std::thread {[this] { ioc_.run(); }};
    }

    ~SubHarness()
    {
        onStrand([this] { client_.disconnect(); });
        work_.reset();
        ioc_.stop();
        thread_.join();
    }

    SubHarness(SubHarness const &) = delete;
    auto operator=(SubHarness const &) -> SubHarness & = delete;

    auto user() const -> std::string const & { return user_; }

    auto connectAndSubscribe() -> void
    {
        onStrand([this] {
            (void)sub_->subscribe();
            (void)client_.connect();
        });
    }

    // Runs f on the strand and waits for it.
    auto onStrand(std::function<void()> f) -> void
    {
        auto done = std::promise<void> {};
        net::post(strand_, [&] {
            f();
            done.set_value();
        });
        done.get_future().wait();
    }

    // History read on the strand; records the outcome.
    auto history(centrifugo::HistoryOptions const &options) -> void
    {
        onStrand([this, options] {
            auto const res = sub_->history(options, [this](auto const &r) {
                update([this, r] {
                    historyResults_.push_back(r);
                });
            });
            if (!res) {
                update([this, code = res.error().ec.value()] { historySendCodes_.push_back(code); });
            }
        });
    }

    // History read, then, in the same strand turn, disconnect or remove the subscription: the
    // read is in flight and must complete exactly once.
    auto historyThen(bool remove) -> void
    {
        onStrand([this, remove] {
            (void)sub_->history({10, std::nullopt, false}, [this](auto const &r) {
                update([this, r] { historyResults_.push_back(r); });
            });
            if (remove) {
                client_.removeSubscription(*sub_);
                sub_ = nullptr;
            } else {
                client_.disconnect();
            }
        });
    }

    auto onStrandDisconnect() -> void
    {
        onStrand([this] { client_.disconnect(); });
    }

    auto reconnect() -> void
    {
        onStrand([this] { (void)client_.connect(); });
    }

    // Registered after the subscription's own handler, so it runs once subscribe was sent.
    auto unsubscribeOnConnected() -> void
    {
        client_.onConnected([this] { sub_->unsubscribe(); });
    }

    auto state() -> centrifugo::SubscriptionState
    {
        auto s = centrifugo::SubscriptionState::UNSUBSCRIBED;
        onStrand([&] { s = sub_->state(); });
        return s;
    }

    auto waitFor(std::string const &what, std::function<bool()> const &pred,
                 std::chrono::seconds timeout = WAIT_TIMEOUT) -> bool
    {
        auto lock = std::unique_lock {mutex_};
        if (cv_.wait_for(lock, timeout, pred)) {
            return true;
        }
        std::cerr << "  FAIL: timed out waiting for " << what << " (received " << fmt(received_)
                  << ", errors " << errors_.size() << ", sub token calls " << subTokenCalls_
                  << ")\n";
        return false;
    }

    template<typename T>
    auto snapshot(T SubHarness::*member) -> T
    {
        auto lock = std::lock_guard {mutex_};
        return this->*member;
    }

    // For waitFor predicates (lock held).
    int subscribed_ {0};
    int unsubscribed_ {0};
    int connectedCount_ {0};
    bool connected_ {false};
    int subTokenCalls_ {0};
    std::vector<int> errors_;
    std::vector<std::string> received_;
    std::vector<std::string> sentSubscribeTokens_;
    std::vector<bool> sentSubscribeRecover_;
    std::vector<int> historySendCodes_;
    std::vector<outcome::result<centrifugo::HistoryResult, centrifugo::Error>> historyResults_;

private:
    auto makeConfig() -> centrifugo::ClientConfig
    {
        auto config = centrifugo::ClientConfig {};
        config.name = "sub-token-test";
        config.getToken = [this]() -> outcome::result<std::string> {
            auto const token = httpRequest(http::verb::get, JWT_PORT,
                                           "/token/" + user_ + "?seconds=300&channels=none_"
                                                   + user_);
            if (!token) {
                return std::make_error_code(std::errc::connection_refused);
            }
            return *token;
        };
        config.logHandler = [this](centrifugo::LogEntry const &entry) {
            if (entry.message != "sending message") {
                return;
            }
            auto lines = std::istringstream {entry.fields.value("message", std::string {})};
            for (auto line = std::string {}; std::getline(lines, line);) {
                auto const j = json::parse(line, nullptr, false);
                if (j.is_object() && j.contains("subscribe")) {
                    auto const &sub = j["subscribe"];
                    update([this, t = sub.value("token", std::string {}),
                            r = sub.value("recover", false)] {
                        sentSubscribeTokens_.push_back(t);
                        sentSubscribeRecover_.push_back(r);
                    });
                }
            }
        };
        return config;
    }

    auto makeSubConfig() -> centrifugo::SubscriptionConfig
    {
        auto config = centrifugo::SubscriptionConfig {};
        if (options_.initialTokenSeconds) {
            config.token = subToken(user_, channel_, *options_.initialTokenSeconds);
        }
        if (options_.subTokenParams) {
            config.getToken = [this](std::string const &channel) -> outcome::result<std::string> {
                auto call = 0;
                update([this, &call] { call = ++subTokenCalls_; });
                auto const params = options_.subTokenParams(call);
                if (params.empty()) {
                    return std::make_error_code(std::errc::permission_denied);
                }
                auto const token = httpRequest(http::verb::get, JWT_PORT,
                                               "/sub-token/" + user_ + "?channel=" + channel + "&"
                                                       + params);
                if (!token) {
                    return std::make_error_code(std::errc::connection_refused);
                }
                return *token;
            };
        }
        return config;
    }

    template<typename F>
    auto update(F &&f) -> void
    {
        {
            auto lock = std::lock_guard {mutex_};
            f();
        }
        cv_.notify_all();
    }

    std::string channel_;
    std::string user_;
    Options options_;
    net::io_context ioc_;
    net::executor_work_guard<net::io_context::executor_type> work_ {ioc_.get_executor()};
    net::strand<net::io_context::executor_type> strand_;
    centrifugo::Client client_;
    centrifugo::Subscription *sub_ {nullptr};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

auto liveChannel() -> std::string
{
    return "live:" + uniqueSuffix();
}

auto constexpr PERMISSION_DENIED = 103;

// A closed namespace refuses a subscribe without a token (103), ending the subscription.
auto testSubTokenMissing() -> bool
{
    auto h = SubHarness {liveChannel(), {false, std::nullopt, {}}};
    h.connectAndSubscribe();
    if (!h.waitFor("unsubscribed", [&] { return h.unsubscribed_ >= 1; })) {
        return false;
    }
    return expectCodes(h.snapshot(&SubHarness::errors_), {PERMISSION_DENIED}, "subscribe errors")
        && expectEq({h.state() == centrifugo::SubscriptionState::UNSUBSCRIBED ? "unsubscribed"
                                                                              : "other"},
                    {"unsubscribed"}, "state");
}

// getToken supplies the token; publications flow; the token went out with subscribe.
auto testSubTokenSubscribe() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel, {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; }) || !publish(channel, "P1")
        || !h.waitFor("P1", [&] { return h.received_.size() >= 1; })) {
        return false;
    }
    auto const tokens = h.snapshot(&SubHarness::sentSubscribeTokens_);
    return expectEq({tokens.size() == 1 && !tokens[0].empty() ? "token sent" : "no token"},
                    {"token sent"}, "subscribe token")
        && expectCodes(h.snapshot(&SubHarness::errors_), {}, "errors");
}

// A static token that has already expired: the server answers 109, getToken gives a fresh one.
auto testSubTokenExpiredRetried() -> bool
{
    auto h = SubHarness {liveChannel(),
                         {true, -60, [](int) { return std::string {"seconds=300"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; })) {
        return false;
    }
    settle();
    // A subscription's 109 must not take the connection-token path (reconnect).
    return expectCodes({h.snapshot(&SubHarness::subTokenCalls_)}, {1}, "sub getToken calls")
        && expectCodes({static_cast<int>(h.snapshot(&SubHarness::sentSubscribeTokens_).size())},
                       {2}, "subscribe commands sent")
        && expectCodes({h.snapshot(&SubHarness::connectedCount_)}, {1}, "connections")
        && expectCodes(h.snapshot(&SubHarness::errors_), {}, "errors");
}

// getToken fails: onError, then UNSUBSCRIBED, nothing sent.
auto testSubTokenGetTokenFails() -> bool
{
    auto h = SubHarness {liveChannel(), {true, std::nullopt, [](int) { return std::string {}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("unsubscribed", [&] { return h.unsubscribed_ >= 1; })) {
        return false;
    }
    auto const unauthorized = static_cast<int>(centrifugo::ErrorType::Unauthorized);
    return expectCodes(h.snapshot(&SubHarness::errors_), {unauthorized}, "errors")
        && expectCodes({static_cast<int>(h.snapshot(&SubHarness::sentSubscribeTokens_).size())},
                       {0}, "subscribe commands sent");
}

// The live_dmd case: a 60 s subscription token is refreshed and the stream keeps flowing.
auto testSubTokenSurvivesTtl() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel, {true, std::nullopt, [](int) { return std::string {"seconds=60"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; })) {
        return false;
    }
    // Past expiry plus the server's grace and check interval (see the control test below).
    std::cout << "  waiting 120 s, past the 60 s token TTL and the server's grace period...\n";
    std::this_thread::sleep_for(120s);
    if (!publish(channel, "LATE") || !h.waitFor("LATE", [&] { return h.received_.size() >= 1; })) {
        return false;
    }
    auto const calls = h.snapshot(&SubHarness::subTokenCalls_);
    if (calls < 2) {
        std::cerr << "  FAIL: expected a refresh getToken call, got " << calls << " call(s)\n";
        return false;
    }
    return expectCodes({h.snapshot(&SubHarness::unsubscribed_)}, {0}, "unsubscribed events")
        && expectCodes(h.snapshot(&SubHarness::errors_), {}, "errors");
}

// Recovery across a reconnect keeps working with a token subscription.
auto testSubTokenRecoveryAcrossReconnect() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel, {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; }) || !publish(channel, "P1")
        || !h.waitFor("P1", [&] { return h.received_.size() >= 1; })) {
        return false;
    }
    // Published while disconnected, so P2 can only arrive through recovery.
    h.onStrandDisconnect();
    if (!h.waitFor("disconnect", [&] { return !h.connected_; }) || !publish(channel, "P2")) {
        return false;
    }
    h.reconnect();
    if (!h.waitFor("resubscribe", [&] { return h.subscribed_ >= 2; })
        || !h.waitFor("P2", [&] { return h.received_.size() >= 2; })) {
        return false;
    }
    settle();
    auto const tokens = h.snapshot(&SubHarness::sentSubscribeTokens_);
    auto const recover = h.snapshot(&SubHarness::sentSubscribeRecover_);
    return expectEq(h.snapshot(&SubHarness::received_), {"P1", "P2"}, "recovered stream")
        && expectEq({recover.size() == 2 && !recover[0] && recover[1] ? "recover on resubscribe"
                                                                      : "no recover"},
                    {"recover on resubscribe"}, "resubscribe recover flag")
        && expectEq({tokens.size() == 2 && !tokens[1].empty() ? "token on resubscribe" : "none"},
                    {"token on resubscribe"}, "resubscribe token");
}

// Control for the TTL test: a 5 s static token, never refreshed. The server unsubscribes it
// once its grace period ends (on its ~27 s presence tick, after 25 s), and the resubscribe with
// the stale token fails without getToken: so the refresh is what keeps the other stream alive.
auto testSubTokenExpiresWithoutRefresh() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel, {true, 5, {}}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; })) {
        return false;
    }
    std::cout << "  waiting up to 90 s for the server to end the expired subscription...\n";
    if (!h.waitFor("unsubscribed", [&] { return h.unsubscribed_ >= 1; }, 90s)
        || !publish(channel, "AFTER")) {
        return false;
    }
    settle();
    auto const unauthorized = static_cast<int>(centrifugo::ErrorType::Unauthorized);
    auto const errors = h.snapshot(&SubHarness::errors_);
    auto const allUnauthorized = !errors.empty()
                              && std::all_of(errors.begin(), errors.end(),
                                             [&](int e) { return e == unauthorized; });
    if (!allUnauthorized) {
        expectCodes(errors, {unauthorized}, "errors (all Unauthorized)");
        return false;
    }
    return expectEq(h.snapshot(&SubHarness::received_), {}, "publications after expiry")
        && expectEq({h.state() == centrifugo::SubscriptionState::UNSUBSCRIBED ? "unsubscribed"
                                                                              : "other"},
                    {"unsubscribed"}, "state");
}

auto historySeqs(centrifugo::HistoryResult const &r) -> std::vector<std::string>
{
    auto out = std::vector<std::string> {};
    for (auto const &pub : r.publications) {
        out.push_back(pub.data.value("seq", std::string {"?"}));
    }
    return out;
}

// history() reads the closed channel's history: oldest first, reversed, since a position.
auto testHistory() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel, {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; }) || !publish(channel, "H1")
        || !publish(channel, "H2") || !publish(channel, "H3")
        || !h.waitFor("H1..H3 live", [&] { return h.received_.size() >= 3; })) {
        return false;
    }

    h.history({10, std::nullopt, false});
    h.history({10, std::nullopt, true});
    h.history({0, std::nullopt, false});
    if (!h.waitFor("3 history results", [&] { return h.historyResults_.size() >= 3; })) {
        return false;
    }
    auto const results = h.snapshot(&SubHarness::historyResults_);
    for (auto const &r : results) {
        if (!r) {
            std::cerr << "  FAIL: history error " << r.error().ec.value() << " "
                      << r.error().message << '\n';
            return false;
        }
    }
    auto const &all = results[0].value();
    auto const ok = expectEq(historySeqs(all), {"H1", "H2", "H3"}, "history limit 10")
                 && expectEq(historySeqs(results[1].value()), {"H3", "H2", "H1"}, "reverse")
                 && expectEq(historySeqs(results[2].value()), {}, "limit 0")
                 && expectCodes({static_cast<int>(results[2].value().offset)},
                                {static_cast<int>(all.publications.back().offset)},
                                "limit 0 position");
    if (!ok) {
        return false;
    }

    h.history({10, centrifugo::StreamPosition {all.publications[0].offset, all.epoch}, false});
    if (!h.waitFor("since result", [&] { return h.historyResults_.size() >= 4; })) {
        return false;
    }
    auto const since = h.snapshot(&SubHarness::historyResults_)[3];
    return since && expectEq(historySeqs(since.value()), {"H2", "H3"}, "since H1");
}

auto testHistoryNotConnected() -> bool
{
    auto h = SubHarness {liveChannel(), {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.history({10, std::nullopt, false});
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(h.snapshot(&SubHarness::historySendCodes_), {notConnected},
                       "history() return")
        && expectCodes({static_cast<int>(h.snapshot(&SubHarness::historyResults_).size())}, {0},
                       "no result callback");
}

// A read in flight when the client disconnects, or the subscription is removed, completes
// exactly once with NotConnected; a later connection does not run it again.
auto runHistoryInterrupted(bool remove) -> bool
{
    auto h = SubHarness {liveChannel(),
                         {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.connectAndSubscribe();
    if (!h.waitFor("subscribed", [&] { return h.subscribed_ >= 1; })) {
        return false;
    }
    h.historyThen(remove);
    if (!h.waitFor("history result", [&] { return h.historyResults_.size() >= 1; })) {
        return false;
    }
    if (!remove) {
        h.reconnect();
        if (!h.waitFor("reconnect", [&] { return h.connectedCount_ >= 2; })) {
            return false;
        }
    }
    settle();
    auto codes = std::vector<int> {};
    for (auto const &r : h.snapshot(&SubHarness::historyResults_)) {
        codes.push_back(r ? 0 : r.error().ec.value());
    }
    auto const notConnected = static_cast<int>(centrifugo::ErrorType::NotConnected);
    return expectCodes(codes, {notConnected}, "history results (exactly once)");
}

auto testHistoryDisconnected() -> bool
{
    return runHistoryInterrupted(false);
}

auto testHistorySubscriptionRemoved() -> bool
{
    return runHistoryInterrupted(true);
}

// unsubscribe() while the subscribe is in flight: unsubscribed at once, and the late subscribe
// reply neither resurrects it nor delivers publications.
auto testUnsubscribeWhileSubscribing() -> bool
{
    auto const channel = liveChannel();
    auto h = SubHarness {channel,
                         {true, std::nullopt, [](int) { return std::string {"seconds=300"}; }}};
    h.unsubscribeOnConnected();
    h.connectAndSubscribe();
    if (!h.waitFor("unsubscribed", [&] { return h.unsubscribed_ >= 1; })
        || !publish(channel, "P1")) {
        return false;
    }
    settle();
    return expectCodes({static_cast<int>(h.snapshot(&SubHarness::sentSubscribeTokens_).size())},
                       {1}, "subscribe sent")
        && expectCodes({h.snapshot(&SubHarness::subscribed_)}, {0}, "subscribed events")
        && expectEq(h.snapshot(&SubHarness::received_), {}, "publications")
        && expectCodes(h.snapshot(&SubHarness::errors_), {}, "errors")
        && expectEq({h.state() == centrifugo::SubscriptionState::UNSUBSCRIBED ? "unsubscribed"
                                                                              : "other"},
                    {"unsubscribed"}, "state");
}
}

// Optional argument: run only the tests whose name contains it.
auto main(int argc, char **argv) -> int
{
    auto const filter = std::string {argc > 1 ? argv[1] : ""};
    auto const tests = std::vector<std::pair<std::string, std::function<bool()>>> {
            {"stream: Client::disconnect + connect", testStreamClientReconnect},
            {"stream: server disconnect, transport reconnect", testStreamTransportReconnect},
            {"stream: application reconnect code 4000", testStreamApplicationReconnect},
            {"stream: token expired, reconnect with fresh token", testStreamTokenExpired},
            {"stream: history overflow is reported", testStreamHistoryOverflow},
            {"stream: terminal disconnect clears positions", testStreamTerminalDisconnect},
            {"cache: client recover drives delivery", testCacheClientRecover},
            {"cache: first-connect delivery via auto_cache_recover",
             testCacheAutoRecoverFirstConnect},
            {"cache: auto_cache_recover on empty channel", testCacheAutoRecoverEmpty},
            {"callbacks: disconnect from onSubscribing", testDisconnectFromSubscribing},
            {"callbacks: disconnect from onUnsubscribed", testDisconnectFromUnsubscribed},
            {"publish: accepted without a subscription", testPublishAccepted},
            {"publish: refused by permissions (103)", testPublishRefused},
            {"publish: failed publish proxy (100)", testPublishProxyFailure},
            {"publish: not connected", testPublishNotConnected},
            {"publish: disconnect before it is sent", testPublishDisconnectBeforeSend},
            {"publish: queued publish dropped with the connection",
             testPublishQueuedAcrossDisconnect},
            {"publish: connection drops before the reply", testPublishReconnectWhileWaiting},
            {"publish: client destroyed before the reply", testPublishClientDestroyed},
            {"sub token: closed namespace without a token (103)", testSubTokenMissing},
            {"sub token: subscribe with getToken", testSubTokenSubscribe},
            {"sub token: expired token retried after 109", testSubTokenExpiredRetried},
            {"sub token: getToken fails", testSubTokenGetTokenFails},
            {"sub token: recovery across a reconnect", testSubTokenRecoveryAcrossReconnect},
            {"history: limit, reverse, position, since", testHistory},
            {"history: not connected", testHistoryNotConnected},
            {"history: connection lost with a read in flight", testHistoryDisconnected},
            {"history: subscription removed with a read in flight", testHistorySubscriptionRemoved},
            {"sub: unsubscribe while the subscribe is in flight", testUnsubscribeWhileSubscribing},
            {"sub token: survives a 60 s TTL", testSubTokenSurvivesTtl},
            {"sub token: static token expires without refresh", testSubTokenExpiresWithoutRefresh},
    };
    auto failed = 0;
    for (auto const &[name, test] : tests) {
        if (name.find(filter) == std::string::npos) {
            continue;
        }
        std::cout << "[" << name << "]\n";
        auto const ok = test();
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        failed += ok ? 0 : 1;
    }
    std::cout << (failed == 0 ? "ALL PASSED" : std::to_string(failed) + " FAILED") << '\n';
    return failed == 0 ? 0 : 1;
}
