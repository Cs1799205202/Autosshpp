module;

#include <argparse/argparse.hpp>
#include <cstdio>

module autosshpp.config;

import std;

namespace autosshpp {

namespace detail {

struct EnvOverrides {
    bool has_monitor_port = false;
    bool has_first_poll = false;
};

struct SanitizedArgv {
    std::vector<char*> argv;
    bool detached_relaunch = false;
};

[[nodiscard]] auto sanitize_argv(int argc, char* argv[]) -> SanitizedArgv {
    SanitizedArgv sanitized;
    sanitized.argv.reserve(static_cast<std::size_t>(argc));

    for (int index = 0; index < argc; ++index) {
        if (index != 0 &&
            std::string_view{argv[index]} == DETACHED_RELAUNCH_MARKER)
        {
            sanitized.detached_relaunch = true;
            continue;
        }

        sanitized.argv.push_back(argv[index]);
    }

    return sanitized;
}

[[nodiscard]] auto parse_control_command(std::string_view text)
    -> std::expected<ControlCommand, std::string>
{
    if (text == "restart")
        return ControlCommand::restart;
    if (text == "stop")
        return ControlCommand::stop;

    return std::unexpected(std::format(
        "unsupported control action \"{}\" (expected restart or stop)",
        text));
}

template <typename T>
[[nodiscard]] auto parse_integer(std::string_view text,
                                 std::string_view label,
                                 T min,
                                 T max) -> std::expected<T, std::string>
{
    static_assert(std::is_integral_v<T>, "parse_integer requires an integral type");

    if (text.starts_with('+'))
        text.remove_prefix(1);

    if (text.empty())
        return std::unexpected(std::format("invalid {} \"{}\"", label, text));

    std::int64_t value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (ec != std::errc{} || ptr != text.data() + text.size())
        return std::unexpected(std::format("invalid {} \"{}\"", label, text));

    if (value < static_cast<std::int64_t>(min) ||
        value > static_cast<std::int64_t>(max))
    {
        return std::unexpected(
            std::format("{} ({}) out of range", label, value));
    }

    return static_cast<T>(value);
}

template <typename Rep, typename Period>
[[nodiscard]] auto parse_duration(std::string_view text,
                                  std::string_view label,
                                  Rep min,
                                  Rep max)
    -> std::expected<std::chrono::duration<Rep, Period>, std::string>
{
    auto parsed = parse_integer<Rep>(text, label, min, max);
    if (!parsed)
        return std::unexpected(parsed.error());
    return std::chrono::duration<Rep, Period>{*parsed};
}

[[nodiscard]] auto parse_monitor_spec(std::string_view spec,
                                      Config& cfg)
    -> std::expected<void, std::string>
{
    cfg.monitor_port = 0;
    cfg.echo_port = 0;
    cfg.monitoring_enabled = true;

    const auto separator = spec.find(':');
    const auto port_text = spec.substr(0, separator);
    auto monitor_port = parse_integer<std::uint16_t>(
        port_text, "monitor port", 0, 65534);
    if (!monitor_port)
        return std::unexpected(monitor_port.error());

    cfg.monitor_port = *monitor_port;
    if (cfg.monitor_port == 0) {
        cfg.monitoring_enabled = false;
        return {};
    }

    if (separator == std::string_view::npos)
        return {};

    auto echo_port = parse_integer<std::uint16_t>(
        spec.substr(separator + 1), "echo port", 1, 65535);
    if (!echo_port)
        return std::unexpected(echo_port.error());

    cfg.echo_port = *echo_port;
    return {};
}

auto get_env(const char* name) -> std::optional<std::string> {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr)
        return std::nullopt;

    auto buf = std::unique_ptr<char, decltype(&std::free)>{raw, &std::free};
    return std::string(buf.get());
#else
    if (auto* v = std::getenv(name); v)
        return std::string(v);
    return std::nullopt;
#endif
}

[[nodiscard]] auto read_env(Config& cfg)
    -> std::expected<EnvOverrides, std::string>
{
    constexpr auto max_int = std::numeric_limits<int>::max();

    EnvOverrides overrides;

    if (auto v = get_env("AUTOSSH_PORT"); v && !v->empty()) {
        auto parsed = parse_monitor_spec(*v, cfg);
        if (!parsed)
            return std::unexpected(parsed.error());
        overrides.has_monitor_port = true;
    }
    if (auto v = get_env("AUTOSSH_POLL")) {
        auto parsed = parse_duration<int, std::ratio<1>>(
            *v, "poll time", 1, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.poll_time = *parsed;
    }
    if (auto v = get_env("AUTOSSH_FIRST_POLL")) {
        auto parsed = parse_duration<int, std::ratio<1>>(
            *v, "first poll time", 1, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.first_poll_time = *parsed;
        overrides.has_first_poll = true;
    }
    if (auto v = get_env("AUTOSSH_GATETIME")) {
        auto parsed = parse_duration<int, std::ratio<1>>(
            *v, "gate time", 0, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.gate_time = *parsed;
    }
    if (auto v = get_env("AUTOSSH_MAXSTART")) {
        auto parsed = parse_integer<int>(*v, "max start number", -1, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.max_starts = *parsed;
    }
    if (auto v = get_env("AUTOSSH_MAXLIFETIME")) {
        auto parsed = parse_duration<int, std::ratio<1>>(
            *v, "max lifetime", 0, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.max_lifetime = *parsed;
    }
    if (auto v = get_env("AUTOSSH_PATH"); v && !v->empty())
        cfg.ssh_path = *v;
    if (auto v = get_env("AUTOSSH_PIDFILE"); v && !v->empty())
        cfg.pid_file = *v;
    if (auto v = get_env("AUTOSSH_LOGFILE"); v && !v->empty())
        cfg.log_file = *v;
    if (auto v = get_env("AUTOSSH_LOGLEVEL")) {
        auto parsed = parse_integer<int>(*v, "log level", 0, 7);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.log_level = *parsed;
    }
    if (auto v = get_env("AUTOSSH_MESSAGE")) {
        if (v->size() > MAX_MESSAGE_BYTES) {
            return std::unexpected(std::format(
                "echo message may only be {} bytes long",
                MAX_MESSAGE_BYTES));
        }
        cfg.message = *v;
    }
    if (get_env("AUTOSSH_DEBUG"))
        cfg.debug = true;

    return overrides;
}

void normalize_timings(Config& cfg, bool has_explicit_first_poll) {
    if (cfg.max_lifetime > std::chrono::seconds{0}) {
        if (cfg.poll_time > cfg.max_lifetime)
            cfg.poll_time = cfg.max_lifetime;
        if (has_explicit_first_poll && cfg.first_poll_time > cfg.max_lifetime)
            cfg.first_poll_time = cfg.max_lifetime;
    }

    const auto half_poll_ms =
        std::chrono::milliseconds{cfg.poll_time.count() * 500LL};
    if (half_poll_ms < cfg.net_timeout)
        cfg.net_timeout = half_poll_ms;
}

}  // namespace detail

auto parse_args(int argc, char* argv[]) -> Config {
    using namespace detail;

    auto sanitized_argv = sanitize_argv(argc, argv);
    constexpr auto max_int = std::numeric_limits<int>::max();

    argparse::ArgumentParser program(
        "autossh", VERSION, argparse::default_arguments::help);
    program.add_description(
        "Automatically restart SSH sessions and tunnels.\n"
        "All unrecognized options are forwarded to ssh.");

    program.add_argument("-V")
        .help("print version and exit")
        .flag();

    program.add_argument("-M")
        .help("monitor port[:echo_port] (0 to disable)")
        .metavar("PORT");

    program.add_argument("-f")
        .help("run in background (daemon mode)")
        .flag();

    program.add_argument("--control")
        .help("send a control action to a running autossh instance")
        .metavar("ACTION");

    program.add_argument("--pid")
        .help("target autossh process id for --control")
        .metavar("PID");

    auto help = [&] {
        std::ostringstream oss;
        oss << program;
        return std::move(oss).str();
    };
    auto fail = [&](std::string_view message) {
        std::println(stderr, "error: {}\n\n{}", message, help());
        std::exit(1);
    };

    std::vector<std::string> unrecognized;
    try {
        unrecognized = program.parse_known_args(
            static_cast<int>(sanitized_argv.argv.size()),
            sanitized_argv.argv.data());
    } catch (const std::exception& e) {
        fail(e.what());
    }

    if (program.get<bool>("-V")) {
        std::println("autossh++ {}", VERSION);
        std::exit(0);
    }

    Config cfg;
    cfg.detached_relaunch = sanitized_argv.detached_relaunch;

    if (program.is_used("--control")) {
        if (program.is_used("-M"))
            fail("-M cannot be used with --control");
        if (program.get<bool>("-f"))
            fail("-f cannot be used with --control");
        if (!unrecognized.empty())
            fail("control mode does not accept ssh arguments");
        if (!program.is_used("--pid"))
            fail("--pid is required with --control");

        auto control_command = parse_control_command(program.get("--control"));
        if (!control_command)
            fail(control_command.error());

        auto control_pid =
            parse_integer<int>(program.get("--pid"), "control pid", 1, max_int);
        if (!control_pid)
            fail(control_pid.error());

        cfg.control_command = *control_command;
        cfg.control_pid = *control_pid;
        return cfg;
    }

    if (program.is_used("--pid"))
        fail("--pid requires --control");

    auto env_overrides = read_env(cfg);
    if (!env_overrides)
        fail(env_overrides.error());

    bool have_port = env_overrides->has_monitor_port;
    if (!have_port && program.is_used("-M")) {
        auto parsed = parse_monitor_spec(program.get("-M"), cfg);
        if (!parsed)
            fail(parsed.error());
        have_port = true;
    }

    if (!have_port)
        fail("-M option is required");

    if (program.get<bool>("-f"))
        cfg.run_as_daemon = true;

    cfg.ssh_args = std::move(unrecognized);

    if (cfg.ssh_args.empty())
        fail("no ssh arguments specified");

    if (cfg.run_as_daemon)
        cfg.gate_time = std::chrono::seconds{0};

    normalize_timings(cfg, env_overrides->has_first_poll);

    return cfg;
}

}  // namespace autosshpp
