#pragma once

#include <string>
#include <system_error>

namespace centrifugo {

struct Error {
    std::error_code ec;
    std::string message;
};

// check error codes at https://centrifugal.dev/docs/server/codes
enum class ErrorType {
    NoError = 0,
    TransportError,
    NotConnected,
    NotDisconnected,
    NotSubscribed,

    Unauthorized,
    NoPing,

    PermissionDenied = 103,
    AlreadySubscribed = 105,
    TokenExpired = 109,

    Shutdown = 3001,

    BadRequest = 3501,
    ForceDisconnect = 3503,
    NotAvailable = 3508,
};

auto make_error_code(ErrorType e) -> std::error_code;

// Boost.Outcome hooks: let `result<T, Error>::value()` throw std::system_error on failure.
inline auto make_error_code(Error const &e) -> std::error_code
{
    return e.ec;
}

inline auto outcome_throw_as_system_error_with_payload(Error const &e) -> void
{
    throw std::system_error {e.ec, e.message};
}

}

namespace std {

template<>
struct is_error_code_enum<centrifugo::ErrorType> : true_type {
};

}
