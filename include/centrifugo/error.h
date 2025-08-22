#pragma once

#include <system_error>

namespace centrifugo {

struct Error {
    std::error_code ec;
    std::string message;
};

enum class ErrorType {
    NoError = 0,
    NotSubscribed = 1,
    TransportError = 2,

    Unauthorized = 3,
    NoPing = 4,

    PermissionDenied = 103,
    AlreadySubscribed = 105,
    TokenExpired = 109,

    Shutdown = 3001,

    BadRequest = 3501,
};

auto make_error_code(ErrorType e) -> std::error_code;

}

namespace std {

template<>
struct is_error_code_enum<centrifugo::ErrorType> : true_type {
};

}
