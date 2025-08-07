#pragma once

#include <system_error>

namespace centrifugo {

struct Error {
    std::error_code ec;
    std::string message;
};

enum class ErrorType {
    NotSubscribed = 1,

    PermissionDenied = 103,
    AlreadySubscribed = 105,
};

auto make_error_code(ErrorType e) -> std::error_code;

}

namespace std {

template<>
struct is_error_code_enum<centrifugo::ErrorType> : true_type {
};

}
