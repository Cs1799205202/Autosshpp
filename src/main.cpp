#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

import std;
import autosshpp.autossh;
import autosshpp.common;
import autosshpp.config;
import autosshpp.platform_control;

namespace {

using autosshpp::Config;
using autosshpp::ErrorCode;
using autosshpp::PrintCommand;
using autosshpp::Result;
using autosshpp::RunCommand;

[[nodiscard]] auto setup_logging(const Config& cfg) -> Result<void> {
    auto level = spdlog::level::info;

    if (cfg.debug) {
        level = spdlog::level::debug;
    } else if (cfg.log_level >= 0) {
        constexpr spdlog::level::level_enum map[] = {
            spdlog::level::critical,
            spdlog::level::critical,
            spdlog::level::critical,
            spdlog::level::err,
            spdlog::level::warn,
            spdlog::level::info,
            spdlog::level::info,
            spdlog::level::debug,
        };
        if (cfg.log_level <= 7)
            level = map[cfg.log_level];
    }

    try {
        if (!cfg.log_file.empty()) {
            spdlog::drop("autossh");
            auto logger = spdlog::basic_logger_mt("autossh", cfg.log_file);
            spdlog::set_default_logger(logger);
        }

        spdlog::set_level(level);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    } catch (const std::exception& e) {
        return std::unexpected(autosshpp::make_error(
            ErrorCode::logging_failure,
            std::format("failed to configure logging: {}", e.what())));
    }

    return {};
}

void print_error(const autosshpp::Error& error) {
    std::println(stderr, "error: {}", autosshpp::format_error(error));
}

}  // namespace

auto main(int argc, char* argv[]) -> int {
    auto parsed = autosshpp::parse_args(argc, argv);
    if (!parsed) {
        print_error(parsed.error());
        return 1;
    }

    if (std::holds_alternative<PrintCommand>(*parsed)) {
        std::println("{}", std::get<PrintCommand>(*parsed).text);
        return 0;
    }

    if (std::holds_alternative<autosshpp::ControlRequest>(*parsed)) {
        auto control_result =
            autosshpp::send_control_request(std::get<autosshpp::ControlRequest>(*parsed));
        if (!control_result) {
            print_error(control_result.error());
            return 1;
        }
        return 0;
    }

    auto cfg = std::move(std::get<RunCommand>(*parsed).config);

    auto detach_result = autosshpp::detach_into_background(cfg);
    if (!detach_result) {
        print_error(detach_result.error());
        return 1;
    }
    if (*detach_result)
        return 0;

    auto logging_result = setup_logging(cfg);
    if (!logging_result) {
        print_error(logging_result.error());
        return 1;
    }

    autosshpp::AutoSSH app(std::move(cfg));
    auto run_result = app.run();
    if (!run_result) {
        print_error(run_result.error());
        return 1;
    }

    return *run_result;
}
