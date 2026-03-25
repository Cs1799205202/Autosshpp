#pragma once

#include <argparse/argparse.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <limits>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace autosshpp {

inline constexpr const char* VERSION = "1.0.0";
inline constexpr int DEFAULT_POLL_TIME = 600;      // seconds
inline constexpr int DEFAULT_GATE_TIME = 30;       // seconds
inline constexpr int DEFAULT_NET_TIMEOUT = 15000;  // milliseconds
inline constexpr int MAX_CONN_TRIES = 3;
inline constexpr int N_FAST_TRIES = 5;
inline constexpr std::size_t MAX_MESSAGE_BYTES = 64;

struct Config {
    // Monitor settings
    uint16_t monitor_port = 0;
    uint16_t echo_port = 0;
    bool monitoring_enabled = true;

    // Timing
    std::chrono::seconds poll_time{DEFAULT_POLL_TIME};
    std::chrono::seconds first_poll_time{0};  // 0 = same as poll_time
    std::chrono::seconds gate_time{DEFAULT_GATE_TIME};
    std::chrono::milliseconds net_timeout{DEFAULT_NET_TIMEOUT};
    std::chrono::seconds max_lifetime{0};  // 0 = unlimited

    // Limits
    int max_starts = -1;  // -1 = unlimited

    // Paths & settings
    std::string ssh_path = "ssh";
    std::string pid_file;
    std::string log_file;
    std::string message;
    int log_level = -1;  // -1 = not set (use default)
    bool debug = false;

    // SSH arguments (everything not consumed by autossh)
    std::vector<std::string> ssh_args;

    // Flags
    bool run_as_daemon = false;

    // Derived helpers
    [[nodiscard]] uint16_t write_port() const { return monitor_port; }
    [[nodiscard]] uint16_t read_port() const { return monitor_port + 1; }

    [[nodiscard]] std::chrono::seconds effective_first_poll() const {
        return first_poll_time > std::chrono::seconds{0} ? first_poll_time : poll_time;
    }
};

// ── Implementation details ──────────────────────────────────────────

namespace detail {

struct EnvOverrides {
    bool has_monitor_port = false;
    bool has_first_poll = false;
};

template <typename T>
[[nodiscard]] auto parse_integer(std::string_view text,
                                 std::string_view label,
                                 T min,
                                 T max) -> std::expected<T, std::string> {
    static_assert(std::is_integral_v<T>, "parse_integer requires an integral type");

    if (text.starts_with('+'))
        text.remove_prefix(1);

    if (text.empty())
        return std::unexpected(
            std::format("invalid {} \"{}\"", label, text));

    std::int64_t value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (ec != std::errc{} || ptr != text.data() + text.size())
        return std::unexpected(
            std::format("invalid {} \"{}\"", label, text));

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
                                  Rep max) -> std::expected<std::chrono::duration<Rep, Period>, std::string> {
    auto parsed = parse_integer<Rep>(text, label, min, max);
    if (!parsed)
        return std::unexpected(parsed.error());
    return std::chrono::duration<Rep, Period>{*parsed};
}

[[nodiscard]] inline auto parse_monitor_spec(std::string_view spec,
                                             Config& cfg) -> std::expected<void, std::string> {
    cfg.monitor_port = 0;
    cfg.echo_port = 0;
    cfg.monitoring_enabled = true;

    const auto separator = spec.find(':');
    const auto port_text = spec.substr(0, separator);
    auto monitor_port =
        parse_integer<std::uint16_t>(port_text, "monitor port", 0, 65534);
    if (!monitor_port)
        return std::unexpected(monitor_port.error());

    cfg.monitor_port = *monitor_port;
    if (cfg.monitor_port == 0) {
        cfg.monitoring_enabled = false;
        return {};
    }

    if (separator == std::string_view::npos)
        return {};

    auto echo_port =
        parse_integer<std::uint16_t>(spec.substr(separator + 1), "echo port", 1, 65535);
    if (!echo_port)
        return std::unexpected(echo_port.error());

    cfg.echo_port = *echo_port;
    return {};
}

inline std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        std::string val(buf);
        free(buf);
        return val;
    }
    return std::nullopt;
#else
    if (auto* v = std::getenv(name); v) return std::string(v);
    return std::nullopt;
#endif
}

[[nodiscard]] inline auto read_env(Config& cfg) -> std::expected<EnvOverrides, std::string> {
    constexpr auto max_int = std::numeric_limits<int>::max();

    EnvOverrides overrides;

    if (auto v = get_env("AUTOSSH_PORT"); v && !v->empty()) {
        auto parsed = parse_monitor_spec(*v, cfg);
        if (!parsed)
            return std::unexpected(parsed.error());
        overrides.has_monitor_port = true;
    }
    if (auto v = get_env("AUTOSSH_POLL")) {
        auto parsed = parse_duration<int, std::ratio<1>>(*v, "poll time", 1, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.poll_time = *parsed;
    }
    if (auto v = get_env("AUTOSSH_FIRST_POLL")) {
        auto parsed = parse_duration<int, std::ratio<1>>(*v, "first poll time", 1, max_int);
        if (!parsed)
            return std::unexpected(parsed.error());
        cfg.first_poll_time = *parsed;
        overrides.has_first_poll = true;
    }
    if (auto v = get_env("AUTOSSH_GATETIME")) {
        auto parsed = parse_duration<int, std::ratio<1>>(*v, "gate time", 0, max_int);
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
        auto parsed = parse_duration<int, std::ratio<1>>(*v, "max lifetime", 0, max_int);
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
            return std::unexpected(
                std::format("echo message may only be {} bytes long",
                            MAX_MESSAGE_BYTES));
        }
        cfg.message = *v;
    }
    if (get_env("AUTOSSH_DEBUG"))
        cfg.debug = true;

    return overrides;
}

inline void normalize_timings(Config& cfg, bool has_explicit_first_poll) {
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

// ── Public parsing entry point ──────────────────────────────────────

[[nodiscard]] inline Config parse_args(int argc, char* argv[]) {
    using namespace detail;

    // Only auto-add --help; we handle version ourselves via -V.
    argparse::ArgumentParser program("autossh", VERSION,
        argparse::default_arguments::help);
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

    // Stringifies the argparse help (argparse only supports ostream).
    auto help = [&] {
        std::ostringstream oss;
        oss << program;
        return std::move(oss).str();
    };
    auto fail = [&](std::string_view message) {
        std::println(stderr, "error: {}\n\n{}", message, help());
        std::exit(1);
    };

    // parse_known_args lets unknown SSH flags (-o, -p, etc.) pass through.
    std::vector<std::string> unrecognized;
    try {
        unrecognized = program.parse_known_args(argc, argv);
    } catch (const std::exception& e) {
        fail(e.what());
    }

    if (program.get<bool>("-V")) {
        std::println("autossh++ {}", VERSION);
        std::exit(0);
    }

    Config cfg;
    auto env_overrides = read_env(cfg);
    if (!env_overrides)
        fail(env_overrides.error());

    // -M: env var AUTOSSH_PORT takes precedence.
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

    // Daemon mode disables gate-time (always retry)
    if (cfg.run_as_daemon)
        cfg.gate_time = std::chrono::seconds{0};

    normalize_timings(cfg, env_overrides->has_first_poll);

    return cfg;
}

}  // namespace autosshpp
