module;

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

export module autosshpp.platform_control;

import std;
import autosshpp.config;

export namespace autosshpp {

enum class RequestedAction {
    none,
    stop,
    restart,
};

[[nodiscard]] auto platform_control_signals() -> std::span<const int>;
[[nodiscard]] auto requested_action_for_signal(int signal_number)
    -> RequestedAction;
[[nodiscard]] auto send_control_request(ControlCommand command, int pid)
    -> std::expected<void, std::string>;
[[nodiscard]] auto detach_into_background(const Config& config)
    -> std::expected<bool, std::string>;

#ifdef _WIN32
struct UniqueHandleCloser {
    using pointer = HANDLE;

    void operator()(HANDLE handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};

using UniqueHandle =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, UniqueHandleCloser>;

[[nodiscard]] auto create_control_event(RequestedAction action, DWORD pid)
    -> std::expected<UniqueHandle, std::string>;
[[nodiscard]] auto request_process_group_interrupt(DWORD pid)
    -> std::expected<void, std::string>;
#endif

}  // namespace autosshpp
