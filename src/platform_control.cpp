#include "platform_control.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <system_error>

#ifdef _WIN32
#  include <csignal>
#else
#  include <csignal>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace autosshpp {

namespace {

#ifdef _WIN32

[[nodiscard]] auto format_windows_error(DWORD error_number) -> std::string {
    return std::system_category().message(static_cast<int>(error_number));
}

[[nodiscard]] auto control_event_name(RequestedAction action, DWORD pid)
    -> std::wstring
{
    std::wstring name = L"Local\\autosshpp-";
    switch (action) {
    case RequestedAction::restart:
        name += L"restart";
        break;
    case RequestedAction::stop:
        name += L"stop";
        break;
    case RequestedAction::none:
        name += L"none";
        break;
    }
    name += L'-';
    name += std::to_wstring(pid);
    return name;
}

#endif

[[nodiscard]] auto signal_number_for_control_command(ControlCommand command)
    -> std::expected<int, std::string>
{
    switch (command) {
    case ControlCommand::restart:
#if defined(SIGUSR1) && !defined(_WIN32)
        return SIGUSR1;
#else
        return std::unexpected(
            "restart control is not available on this platform");
#endif
    case ControlCommand::stop:
        return SIGTERM;
    case ControlCommand::none:
        return std::unexpected("no control action requested");
    }

    return std::unexpected("unknown control action");
}

[[nodiscard]] auto requested_action_for_control_command(ControlCommand command)
    -> std::expected<RequestedAction, std::string>
{
    switch (command) {
    case ControlCommand::restart:
        return RequestedAction::restart;
    case ControlCommand::stop:
        return RequestedAction::stop;
    case ControlCommand::none:
        return std::unexpected("no control action requested");
    }

    return std::unexpected("unknown control action");
}

}  // namespace

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

auto send_control_request(ControlCommand command, int pid)
    -> std::expected<void, std::string>
{
    if (pid <= 0) {
        return std::unexpected(std::format(
            "control pid ({}) must be positive", pid));
    }

#ifdef _WIN32
    auto action = requested_action_for_control_command(command);
    if (!action)
        return std::unexpected(action.error());

    const auto event_name =
        control_event_name(*action, static_cast<DWORD>(pid));
    HANDLE event_handle =
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
    if (!event_handle) {
        const auto error_number = ::GetLastError();
        return std::unexpected(std::format(
            "failed to open control event for pid {}: {}",
            pid,
            format_windows_error(error_number)));
    }

    if (!::SetEvent(event_handle)) {
        const auto error_number = ::GetLastError();
        ::CloseHandle(event_handle);
        return std::unexpected(std::format(
            "failed to signal control event for pid {}: {}",
            pid,
            format_windows_error(error_number)));
    }

    ::CloseHandle(event_handle);
    return {};
#else
    auto signal_number = signal_number_for_control_command(command);
    if (!signal_number)
        return std::unexpected(signal_number.error());

    if (::kill(pid, *signal_number) != 0) {
        return std::unexpected(std::format(
            "failed to send control signal to pid {}: {}",
            pid,
            std::strerror(errno)));
    }

    return {};
#endif
}

auto detach_into_background(const Config& config)
    -> std::expected<bool, std::string>
{
    if (!config.run_as_daemon)
        return false;

#ifdef _WIN32
    if (config.detached_relaunch)
        return false;

    std::wstring command_line = ::GetCommandLineW();
    if (!command_line.empty())
        command_line += L' ';
    command_line.append(
        DETACHED_RELAUNCH_MARKER.begin(), DETACHED_RELAUNCH_MARKER.end());

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);

    PROCESS_INFORMATION process_info{};
    constexpr DWORD creation_flags =
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

    if (!::CreateProcessW(nullptr,
                          command_line.data(),
                          nullptr,
                          nullptr,
                          FALSE,
                          creation_flags,
                          nullptr,
                          nullptr,
                          &startup_info,
                          &process_info))
    {
        const auto error_number = ::GetLastError();
        return std::unexpected(std::format(
            "failed to relaunch detached background process for -f: {}",
            format_windows_error(error_number)));
    }

    ::CloseHandle(process_info.hThread);
    ::CloseHandle(process_info.hProcess);
    return true;
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

#ifdef _WIN32
auto create_control_event(RequestedAction action, DWORD pid)
    -> std::expected<HANDLE, std::string>
{
    const auto event_name = control_event_name(action, pid);
    HANDLE event_handle =
        ::CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    if (!event_handle) {
        const auto error_number = ::GetLastError();
        return std::unexpected(std::format(
            "failed to create control event for pid {}: {}",
            pid,
            format_windows_error(error_number)));
    }

    return event_handle;
}
#endif

}  // namespace autosshpp
