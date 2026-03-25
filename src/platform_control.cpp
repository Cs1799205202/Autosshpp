#include "platform_control.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#ifdef _WIN32
#  include <csignal>
#else
#  include <csignal>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace autosshpp {

auto platform_control_signals() -> std::span<const int> {
#ifdef _WIN32
    static constexpr int signals[] = {SIGINT, SIGTERM};
#elif defined(SIGUSR1)
    static constexpr int signals[] = {SIGINT, SIGTERM, SIGUSR1};
#else
    static constexpr int signals[] = {SIGINT, SIGTERM};
#endif

    return signals;
}

auto requested_action_for_signal(int signal_number) -> RequestedAction {
#if defined(SIGUSR1) && !defined(_WIN32)
    if (signal_number == SIGUSR1)
        return RequestedAction::restart;
#endif

    switch (signal_number) {
    case SIGINT:
    case SIGTERM:
        return RequestedAction::stop;
    default:
        return RequestedAction::none;
    }
}

auto detach_into_background(const Config& config)
    -> std::expected<bool, std::string>
{
    if (!config.run_as_daemon)
        return false;

#ifdef _WIN32
    return std::unexpected(
        "real -f background mode is not implemented on Windows yet");
#else
    struct DaemonStatus {
        int error_number = 0;
        bool success = false;
    };

    int status_pipe[2] = {-1, -1};
    if (::pipe(status_pipe) != 0) {
        return std::unexpected(std::format(
            "failed to create daemon status pipe: {}", std::strerror(errno)));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const auto error_number = errno;
        ::close(status_pipe[0]);
        ::close(status_pipe[1]);
        return std::unexpected(std::format(
            "failed to fork for -f: {}", std::strerror(error_number)));
    }

    if (pid > 0) {
        ::close(status_pipe[1]);

        DaemonStatus status;
        ssize_t bytes_read = 0;
        do {
            bytes_read = ::read(status_pipe[0], &status, sizeof(status));
        } while (bytes_read < 0 && errno == EINTR);

        ::close(status_pipe[0]);

        if (bytes_read == 0) {
            return std::unexpected(
                "detached child exited before reporting startup status");
        }
        if (bytes_read != static_cast<ssize_t>(sizeof(status))) {
            return std::unexpected(
                "detached child reported an invalid startup status");
        }
        if (!status.success) {
            return std::unexpected(std::format(
                "failed to enter background mode: {}",
                std::strerror(status.error_number)));
        }

        return true;
    }

    ::close(status_pipe[0]);

    auto report_and_exit = [&](int error_number) -> void {
        const DaemonStatus status{
            .error_number = error_number,
            .success = false,
        };
        [[maybe_unused]] const auto bytes_written =
            ::write(status_pipe[1], &status, sizeof(status));
        ::close(status_pipe[1]);
        _exit(1);
    };

    if (::setsid() < 0)
        report_and_exit(errno);

    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull < 0)
        report_and_exit(errno);

    for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(devnull, fd) < 0) {
            const auto error_number = errno;
            if (devnull > STDERR_FILENO)
                ::close(devnull);
            report_and_exit(error_number);
        }
    }

    if (devnull > STDERR_FILENO)
        ::close(devnull);

    const DaemonStatus status{
        .error_number = 0,
        .success = true,
    };
    [[maybe_unused]] const auto bytes_written =
        ::write(status_pipe[1], &status, sizeof(status));
    ::close(status_pipe[1]);

    return false;
#endif
}

}  // namespace autosshpp
