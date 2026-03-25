#pragma once

#include <expected>
#include <span>
#include <string>

#include "config.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace autosshpp {

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
[[nodiscard]] auto create_control_event(RequestedAction action, DWORD pid)
    -> std::expected<HANDLE, std::string>;
#endif

}  // namespace autosshpp
