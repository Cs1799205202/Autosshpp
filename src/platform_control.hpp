#pragma once

#include <expected>
#include <span>
#include <string>

#include "config.hpp"

namespace autosshpp {

enum class RequestedAction {
    none,
    stop,
    restart,
};

[[nodiscard]] auto platform_control_signals() -> std::span<const int>;
[[nodiscard]] auto requested_action_for_signal(int signal_number)
    -> RequestedAction;
[[nodiscard]] auto detach_into_background(const Config& config)
    -> std::expected<bool, std::string>;

}  // namespace autosshpp
