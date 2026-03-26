export module autosshpp.common;

import std;

export namespace autosshpp {

enum class ErrorCode {
    invalid_argument,
    missing_argument,
    unsupported_operation,
    io_failure,
    platform_failure,
    logging_failure,
    process_failure,
    pid_file_failure,
    internal_failure,
};

struct Error {
    ErrorCode code = ErrorCode::internal_failure;
    std::string message;
    std::string detail;
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline auto make_error(ErrorCode code,
                                     std::string message,
                                     std::string detail = {}) -> Error
{
    return Error{
        .code = code,
        .message = std::move(message),
        .detail = std::move(detail),
    };
}

[[nodiscard]] inline auto format_error(const Error& error) -> std::string {
    if (error.detail.empty())
        return error.message;

    return std::format("{}\n\n{}", error.message, error.detail);
}

}  // namespace autosshpp
