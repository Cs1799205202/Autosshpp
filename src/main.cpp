#include <boost/asio.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "config.hpp"
#include "autossh.hpp"
#include "platform_control.hpp"

import std;

static void setup_logging(const autosshpp::Config& cfg) {
    auto level = spdlog::level::info;

    if (cfg.debug) {
        level = spdlog::level::debug;
    } else if (cfg.log_level >= 0) {
        // Map syslog-style levels (0 = emergency ... 7 = debug) to spdlog.
        constexpr spdlog::level::level_enum map[] = {
            spdlog::level::critical,  // 0 - emergency
            spdlog::level::critical,  // 1 - alert
            spdlog::level::critical,  // 2 - critical
            spdlog::level::err,       // 3 - error
            spdlog::level::warn,      // 4 - warning
            spdlog::level::info,      // 5 - notice
            spdlog::level::info,      // 6 - info
            spdlog::level::debug,     // 7 - debug
        };
        if (cfg.log_level <= 7)
            level = map[cfg.log_level];
    }

    if (!cfg.log_file.empty()) {
        auto logger = spdlog::basic_logger_mt("autossh", cfg.log_file);
        spdlog::set_default_logger(logger);
    }

    spdlog::set_level(level);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

auto main(int argc, char* argv[]) -> int {
    auto cfg = autosshpp::parse_args(argc, argv);

    auto detach_result = autosshpp::detach_into_background(cfg);
    if (!detach_result) {
        std::println(stderr, "error: {}", detach_result.error());
        return 1;
    }
    if (*detach_result)
        return 0;

    setup_logging(cfg);

    boost::asio::io_context io;
    autosshpp::AutoSSH app(io, std::move(cfg));
    app.run();

    return app.exit_code();
}
