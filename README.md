# Centrifugo C++ Client

A modern C++17 client library for [Centrifugo](https://github.com/centrifugal/centrifugo) real-time messaging server. This library provides an asynchronous, WebSocket-based client implementation using Boost.Beast and Boost.Asio.

## Features

- 🚀 **Asynchronous I/O** - Built on Boost.Asio for high-performance networking
- 🔐 **JWT Authentication** - Full support for JWT token-based authentication
- 📡 **Real-time Subscriptions** - Subscribe to channels and receive publications
- 🎟️ **Subscription Tokens** - Closed channels via per-channel tokens, refreshed before expiry
- 🕘 **History** - Read a channel's retained publications
- 🔄 **Automatic Reconnection** - Configurable reconnection logic with exponential backoff
- 📝 **Modern C++17** - Clean, type-safe API using modern C++ features
- 🛡️ **Error Handling** - Comprehensive error handling with boost::outcome
- 📊 **Logging Support** - Configurable logging with structured log entries

## Requirements

- **C++17** or later
- **CMake 3.17** or later
- **OpenSSL**

Can be automatically installed by CPM:
- **Boost** (ASIO, Beast, Outcome, Signals2, System, URL)
- **nlohmann/json**

## Installation

### Using CMake

```bash
cmake -S . -B build
cmake --build build
```

### Building Examples

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build
```

## Examples

The `examples/` directory contains complete working examples:

- **[`full.cpp`](examples/full.cpp)** - Complete example with JWT authentication, subscriptions, and event handling
- **[`staging.cpp`](examples/staging.cpp)** - Staging environment example

## Development Environment

This project uses [devenv](https://devenv.sh) for development environment management:

```bash
# Install devenv (if not already installed)
# Then run:
devenv shell
```

The project includes Docker services for development:

```bash
docker compose up -d # Starts Centrifugo server and JWT generator service to be used with "full" example
```

## Integration Tests

`tests/recovery_test.cpp` checks history recovery for server-side subscriptions (channels granted
by the connection token) against a real Centrifugo (`stream:`, `cache:` and `cachec:` namespaces in
`services/centrifugo/config.yaml`), including server-forced reconnects and token expiry, plus
subscription tokens and history on a closed `live:` namespace. It needs
Docker and is built only in a top-level build (CTest label `integration`):

```bash
cmake -S . -B build -DCENTRIFUGO_CPP_BUILD_TESTS=ON
cmake --build build
tests/run_integration.sh   # private compose project on free 127.0.0.1 ports; runs test; tears down
```

## Server-side Subscriptions

Channels granted by the connection token's `channels` claim are recovered automatically on
reconnect. For those channels, `onSubscribed` and any recovered `onPublication` calls fire
**before** `onConnected`. If the server cannot recover a gap, the client logs a
`LogLevel::Error` entry ("server-side subscription not recovered"); publications may have been
missed. Handlers should be idempotent.

## Subscription Tokens and History

Channels the server does not open to every client (`allow_subscribe_for_client: false`) need a
subscription token. Pass a `SubscriptionConfig`:

```cpp
auto config = centrifugo::SubscriptionConfig {};
config.getToken = [](std::string const &channel) -> outcome::result<std::string> {
    return fetchSubscriptionToken(channel); // your backend; runs on the client's strand
};
auto sub = client.newSubscription("live_dmd:" + machineUuid, std::move(config));
```

- `token` is used for the first subscribe; `getToken` supplies the rest.
- Tokens are refreshed before they expire (`sub_refresh`): at 3/4 of a short TTL, otherwise
  `refreshTokenBeforeExpiry` ahead. A 60 s TTL refreshes after 45 s.
- A token-expired reply (109) to subscribe is retried once with a fresh token. If `getToken`
  fails, or the server refuses for good (e.g. 103), `onError` fires and the subscription ends
  `UNSUBSCRIBED`, keeping its stream position for a later `subscribe()`.

`Subscription::history()` reads retained publications, oldest first unless `reverse`:

```cpp
sub.history({10}, [](outcome::result<centrifugo::HistoryResult, centrifugo::Error> const &r) {
    if (!r) { return; } // r.error(): server error, or NotConnected if the connection dropped
    for (auto const &pub : r.value().publications) { /* ... */ }
});
```

`limit` 0 returns only the stream position; `since` limits it to publications after one. The
callback runs exactly once, on the client's strand; only a read still pending when the client is
destroyed completes inside `~Client`, on that thread.

Client-side subscriptions also follow the server: an `unsubscribe` push with code 2500 or above
(e.g. an expired token) subscribes again from the kept position; a lower code ends the
subscription. `unsubscribe()` takes effect at once, and `removeSubscription()` is safe from the
subscription's own callbacks.

## Architecture

### Core Components

- **Client** - Main client class managing connections and subscriptions
- **Subscription** - Individual channel subscription management
- **Transport** - WebSocket transport layer using Boost.Beast
- **Protocol** - Centrifugo protocol implementation
- **Error** - Comprehensive error handling system

### Dependencies

The project uses CMake with CPM (CMake Package Manager) for dependency management:

- **Boost** - Networking, WebSocket, and utilities
- **OpenSSL** - TLS/SSL support
- **nlohmann/json** - JSON serialization/deserialization


## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2025 Spinner Systems, Inc. (DBA Scorbit), scrobit.io, All Rights Reserved
## Links

- [Centrifugo Server](https://github.com/centrifugal/centrifugo)
- [Centrifugo Documentation](https://centrifugal.dev)
- [Boost Libraries](https://www.boost.org)
