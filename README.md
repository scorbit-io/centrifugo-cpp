# Centrifugo C++ Client

A modern C++17 client library for [Centrifugo](https://github.com/centrifugal/centrifugo) real-time messaging server. This library provides an asynchronous, WebSocket-based client implementation using Boost.Beast and Boost.Asio.

## Features

- 🚀 **Asynchronous I/O** - Built on Boost.Asio for high-performance networking
- 🔐 **JWT Authentication** - Full support for JWT token-based authentication
- 📡 **Real-time Subscriptions** - Subscribe to channels and receive publications
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
`services/centrifugo/config.yaml`), including server-forced reconnects and token expiry. It needs
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
