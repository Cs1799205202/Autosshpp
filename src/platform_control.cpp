module;

#include <cerrno>
#include <cstring>
#include <format>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <csignal>
#else
#  include <csignal>
#  include <fcntl.h>
#  include <unistd.h>
#endif

module autosshpp.platform_control;

import std;
import autosshpp.common;
import autosshpp.config;

namespace autosshpp {

namespace {

#ifdef _WIN32

[[nodiscard]] auto format_windows_error(DWORD error_number) -> std::string {
    return std::system_category().message(static_cast<int>(error_number));
}

[[nodiscard]] auto make_windows_error(ErrorCode code,
                                      std::string message,
                                      DWORD error_number) -> Error
{
    return make_error(
        code,
        std::format("{}: {}", std::move(message), format_windows_error(error_number)));
}

class ScopedCtrlHandlerIgnore {
public:
    ScopedCtrlHandlerIgnore() = default;

    ScopedCtrlHandlerIgnore(const ScopedCtrlHandlerIgnore&) = delete;
    auto operator=(const ScopedCtrlHandlerIgnore&)
        -> ScopedCtrlHandlerIgnore& = delete;

    ScopedCtrlHandlerIgnore(ScopedCtrlHandlerIgnore&& other) noexcept
        : active_(std::exchange(other.active_, false))
    {
    }

    auto operator=(ScopedCtrlHandlerIgnore&& other) noexcept
        -> ScopedCtrlHandlerIgnore&
    {
        if (this == &other)
            return *this;

        reset();
        active_ = std::exchange(other.active_, false);
        return *this;
    }

    ~ScopedCtrlHandlerIgnore() {
        reset();
    }

    [[nodiscard]] static auto create() -> Result<ScopedCtrlHandlerIgnore> {
        if (!::SetConsoleCtrlHandler(nullptr, TRUE)) {
            const auto error_number = ::GetLastError();
            return std::unexpected(make_windows_error(
                ErrorCode::platform_failure,
                "failed to ignore console control events temporarily",
                error_number));
        }

        ScopedCtrlHandlerIgnore guard;
        guard.active_ = true;
        return guard;
    }

private:
    void reset() noexcept {
        if (!active_)
            return;

        ::SetConsoleCtrlHandler(nullptr, FALSE);
        active_ = false;
    }

    bool active_ = false;
};

class ScopedConsoleAttachment {
public:
    ScopedConsoleAttachment() = default;

    ScopedConsoleAttachment(const ScopedConsoleAttachment&) = delete;
    auto operator=(const ScopedConsoleAttachment&)
        -> ScopedConsoleAttachment& = delete;

    ScopedConsoleAttachment(ScopedConsoleAttachment&& other) noexcept
        : detached_existing_console_(
              std::exchange(other.detached_existing_console_, false))
        , attached_target_console_(
              std::exchange(other.attached_target_console_, false))
    {
    }

    auto operator=(ScopedConsoleAttachment&& other) noexcept
        -> ScopedConsoleAttachment&
    {
        if (this == &other)
            return *this;

        reset();
        detached_existing_console_ =
            std::exchange(other.detached_existing_console_, false);
        attached_target_console_ =
            std::exchange(other.attached_target_console_, false);
        return *this;
    }

    ~ScopedConsoleAttachment() {
        reset();
    }

    [[nodiscard]] static auto attach_to_process_console(DWORD pid)
        -> Result<ScopedConsoleAttachment>
    {
        ScopedConsoleAttachment attachment;

        if (::GetConsoleWindow() != nullptr) {
            if (!::FreeConsole()) {
                const auto error_number = ::GetLastError();
                return std::unexpected(make_windows_error(
                    ErrorCode::platform_failure,
                    "failed to detach from current console",
                    error_number));
            }
            attachment.detached_existing_console_ = true;
        }

        if (!::AttachConsole(pid)) {
            const auto error_number = ::GetLastError();
            if (attachment.detached_existing_console_)
                attachment.reattach_parent_console();
            return std::unexpected(make_windows_error(
                ErrorCode::platform_failure,
                std::format("failed to attach to child console for pid {}", pid),
                error_number));
        }

        attachment.attached_target_console_ = true;
        return attachment;
    }

private:
    void reset() noexcept {
        if (attached_target_console_) {
            ::FreeConsole();
            attached_target_console_ = false;
        }

        if (detached_existing_console_) {
            reattach_parent_console();
            detached_existing_console_ = false;
        }
    }

    void reattach_parent_console() noexcept {
        [[maybe_unused]] const auto reattached =
            ::AttachConsole(ATTACH_PARENT_PROCESS);
    }

    bool detached_existing_console_ = false;
    bool attached_target_console_ = false;
};

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

[[nodiscard]] auto make_errno_error(ErrorCode code,
                                    std::string message,
                                    int error_number = errno) -> Error
{
    return make_error(
        code,
        std::format("{}: {}", std::move(message), std::strerror(error_number)));
}

[[nodiscard]] auto signal_number_for_control_command(ControlCommand command)
    -> Result<int>
{
    switch (command) {
    case ControlCommand::restart:
#if defined(SIGUSR1) && !defined(_WIN32)
        return SIGUSR1;
#else
        return std::unexpected(make_error(
            ErrorCode::unsupported_operation,
            "restart control is not available on this platform"));
#endif
    case ControlCommand::stop:
        return SIGTERM;
    case ControlCommand::none:
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "no control action requested"));
    }

    return std::unexpected(make_error(
        ErrorCode::internal_failure,
        "unknown control action"));
}

[[nodiscard]] auto requested_action_for_control_command(ControlCommand command)
    -> Result<RequestedAction>
{
    switch (command) {
    case ControlCommand::restart:
        return RequestedAction::restart;
    case ControlCommand::stop:
        return RequestedAction::stop;
    case ControlCommand::none:
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "no control action requested"));
    }

    return std::unexpected(make_error(
        ErrorCode::internal_failure,
        "unknown control action"));
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

auto send_control_request(const ControlRequest& request) -> Result<void> {
    if (request.pid <= 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            std::format("control pid ({}) must be positive", request.pid)));
    }

#ifdef _WIN32
    auto action = requested_action_for_control_command(request.command);
    if (!action)
        return std::unexpected(action.error());

    const auto event_name =
        control_event_name(*action, static_cast<DWORD>(request.pid));
    auto event_handle = UniqueHandle{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str())};
    if (!event_handle) {
        const auto error_number = ::GetLastError();
        return std::unexpected(make_windows_error(
            ErrorCode::platform_failure,
            std::format("failed to open control event for pid {}", request.pid),
            error_number));
    }

    if (!::SetEvent(event_handle.get())) {
        const auto error_number = ::GetLastError();
        return std::unexpected(make_windows_error(
            ErrorCode::platform_failure,
            std::format("failed to signal control event for pid {}", request.pid),
            error_number));
    }

    return {};
#else
    auto signal_number = signal_number_for_control_command(request.command);
    if (!signal_number)
        return std::unexpected(signal_number.error());

    if (::kill(request.pid, *signal_number) != 0) {
        return std::unexpected(make_errno_error(
            ErrorCode::platform_failure,
            std::format("failed to send control signal to pid {}", request.pid)));
    }

    return {};
#endif
}

auto detach_into_background(const Config& config) -> Result<bool> {
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
        return std::unexpected(make_windows_error(
            ErrorCode::platform_failure,
            "failed to relaunch detached background process for -f",
            error_number));
    }

    [[maybe_unused]] auto thread_handle = UniqueHandle{process_info.hThread};
    [[maybe_unused]] auto process_handle = UniqueHandle{process_info.hProcess};
    return true;
#else
    struct DaemonStatus {
        int error_number = 0;
        bool success = false;
    };

    int status_pipe[2] = {-1, -1};
    if (::pipe(status_pipe) != 0) {
        return std::unexpected(make_errno_error(
            ErrorCode::platform_failure,
            "failed to create daemon status pipe"));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const auto error_number = errno;
        ::close(status_pipe[0]);
        ::close(status_pipe[1]);
        return std::unexpected(make_errno_error(
            ErrorCode::platform_failure,
            "failed to fork for -f",
            error_number));
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
            return std::unexpected(make_error(
                ErrorCode::platform_failure,
                "detached child exited before reporting startup status"));
        }
        if (bytes_read != static_cast<ssize_t>(sizeof(status))) {
            return std::unexpected(make_error(
                ErrorCode::platform_failure,
                "detached child reported an invalid startup status"));
        }
        if (!status.success) {
            return std::unexpected(make_errno_error(
                ErrorCode::platform_failure,
                "failed to enter background mode",
                status.error_number));
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
    -> Result<UniqueHandle>
{
    const auto event_name = control_event_name(action, pid);
    auto event_handle = UniqueHandle{
        ::CreateEventW(nullptr, FALSE, FALSE, event_name.c_str())};
    if (!event_handle) {
        const auto error_number = ::GetLastError();
        return std::unexpected(make_windows_error(
            ErrorCode::platform_failure,
            std::format("failed to create control event for pid {}", pid),
            error_number));
    }

    return event_handle;
}

auto request_process_group_interrupt(DWORD pid) -> Result<void> {
    if (pid == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "child pid must be non-zero"));
    }

    auto ignored_ctrl_handler = ScopedCtrlHandlerIgnore::create();
    if (!ignored_ctrl_handler)
        return std::unexpected(ignored_ctrl_handler.error());

    if (::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid))
        return {};

    const auto direct_error = ::GetLastError();
    auto console_attachment = ScopedConsoleAttachment::attach_to_process_console(pid);
    if (!console_attachment) {
        return std::unexpected(make_error(
            ErrorCode::platform_failure,
            std::format(
                "failed to request graceful console shutdown for pid {}: {}; "
                "attach fallback failed: {}",
                pid,
                format_windows_error(direct_error),
                console_attachment.error().message)));
    }

    if (!::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)) {
        const auto attached_error = ::GetLastError();
        return std::unexpected(make_windows_error(
            ErrorCode::platform_failure,
            std::format("failed to request graceful console shutdown for pid {}", pid),
            attached_error));
    }

    return {};
}
#endif

}  // namespace autosshpp
