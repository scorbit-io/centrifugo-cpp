// Integration test: history recovery for server-side (JWT `channels` claim) subscriptions.
// Needs Centrifugo + jwt-generator from docker-compose.yml; run via tests/run_integration.sh,
// which exports CENTRIFUGO_PORT and JWT_PORT (defaults 8000 / 3001).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
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
        client_.onConnected([this] { update([this] { connected_ = true; }); });
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

    // Only for waitFor predicates, which run with the lock held.
    auto receivedCount() const -> std::size_t { return received_.size(); }
    auto subscribedCount() const -> int { return subscribedCount_; }
    auto disconnectedCount() const -> int { return disconnectedCount_; }
    auto connected() const -> bool { return connected_; }

private:
    auto makeConfig() -> centrifugo::ClientConfig
    {
        auto config = centrifugo::ClientConfig {};
        config.name = "recovery-test";
        config.getToken = [this]() -> outcome::result<std::string> {
            auto call = 0;
            update([this, &call] { call = ++tokenCalls_; });
            auto const params = tokenParams_ ? tokenParams_(call) : std::string {"seconds=300"};
            auto const token = httpRequest(http::verb::get, JWT_PORT,
                                           "/token/" + user_ + "?channels=" + channel_ + "&"
                                                   + params);
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
    bool connected_ {false};
    int subscribedCount_ {0};
    int disconnectedCount_ {0};
    int notRecoveredLogs_ {0};
    int tokenCalls_ {0};
    std::vector<std::string> received_;
    std::vector<std::uint64_t> offsets_;
    std::vector<int> errorCodes_;
    std::vector<json> connectSubs_;
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

}

auto main() -> int
{
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
    };
    auto failed = 0;
    for (auto const &[name, test] : tests) {
        std::cout << "[" << name << "]\n";
        auto const ok = test();
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        failed += ok ? 0 : 1;
    }
    std::cout << (failed == 0 ? "ALL PASSED" : std::to_string(failed) + " FAILED") << '\n';
    return failed == 0 ? 0 : 1;
}
