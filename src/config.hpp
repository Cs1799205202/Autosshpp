#pragma once

#include <argparse/argparse.hpp>
#include <print>

namespace autosshpp {

inline constexpr const char* VERSION = "1.0.0";
inline constexpr int DEFAULT_POLL_TIME = 600;      // seconds
inline constexpr int DEFAULT_GATE_TIME = 30;       // seconds
inline constexpr int DEFAULT_NET_TIMEOUT = 15000;  // milliseconds
inline constexpr int MAX_CONN_TRIES = 3;
inline constexpr int N_FAST_TRIES = 5;

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

inline void parse_monitor_spec(const std::string& spec, Config& cfg) {
    if (auto pos = spec.find(':'); pos != std::string::npos) {
        cfg.monitor_port = static_cast<uint16_t>(std::stoul(spec.substr(0, pos)));
        cfg.echo_port    = static_cast<uint16_t>(std::stoul(spec.substr(pos + 1)));
    } else {
        cfg.monitor_port = static_cast<uint16_t>(std::stoul(spec));
    }

    if (cfg.monitor_port == 0)
        cfg.monitoring_enabled = false;
    else if (cfg.monitor_port > 65534)
        throw std::runtime_error(
            std::format("monitor port ({}) out of range", cfg.monitor_port));
}

inline std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        std::string val(buf);
        free(buf);
        if (!val.empty()) return val;
    }
    return std::nullopt;
#else
    if (auto* v = std::getenv(name); v && *v) return std::string(v);
    return std::nullopt;
#endif
}

inline void read_env(Config& cfg) {
    if (auto v = get_env("AUTOSSH_PORT"))
        parse_monitor_spec(*v, cfg);
    if (auto v = get_env("AUTOSSH_POLL"))
        cfg.poll_time = std::chrono::seconds(std::stoi(*v));
    if (auto v = get_env("AUTOSSH_FIRST_POLL"))
        cfg.first_poll_time = std::chrono::seconds(std::stoi(*v));
    if (auto v = get_env("AUTOSSH_GATETIME"))
        cfg.gate_time = std::chrono::seconds(std::stoi(*v));
    if (auto v = get_env("AUTOSSH_MAXSTART"))
        cfg.max_starts = std::stoi(*v);
    if (auto v = get_env("AUTOSSH_MAXLIFETIME"))
        cfg.max_lifetime = std::chrono::seconds(std::stoi(*v));
    if (auto v = get_env("AUTOSSH_PATH"))
        cfg.ssh_path = *v;
    if (auto v = get_env("AUTOSSH_PIDFILE"))
        cfg.pid_file = *v;
    if (auto v = get_env("AUTOSSH_LOGFILE"))
        cfg.log_file = *v;
    if (auto v = get_env("AUTOSSH_LOGLEVEL"))
        cfg.log_level = std::stoi(*v);
    if (auto v = get_env("AUTOSSH_MESSAGE"))
        cfg.message = *v;
    if (get_env("AUTOSSH_DEBUG"))
        cfg.debug = true;
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

    // parse_known_args lets unknown SSH flags (-o, -p, etc.) pass through.
    std::vector<std::string> unrecognized;
    try {
        unrecognized = program.parse_known_args(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "{}\n\n{}", e.what(), help());
        std::exit(1);
    }

    if (program.get<bool>("-V")) {
        std::println("autossh++ {}", VERSION);
        std::exit(0);
    }

    Config cfg;
    read_env(cfg);

    // -M: env var AUTOSSH_PORT takes precedence.
    bool have_port = false;
    if (get_env("AUTOSSH_PORT")) {
        have_port = true;  // already applied in read_env
    } else if (program.is_used("-M")) {
        parse_monitor_spec(program.get("-M"), cfg);
        have_port = true;
    }

    if (!have_port) {
        std::println(stderr, "error: -M option is required\n\n{}", help());
        std::exit(1);
    }

    if (program.get<bool>("-f"))
        cfg.run_as_daemon = true;

    cfg.ssh_args = std::move(unrecognized);

    if (cfg.ssh_args.empty()) {
        std::println(stderr, "error: no ssh arguments specified\n\n{}", help());
        std::exit(1);
    }

    // Adjust net timeout for short poll times
    auto half_poll_ms = cfg.poll_time.count() * 500;
    if (half_poll_ms < cfg.net_timeout.count())
        cfg.net_timeout = std::chrono::milliseconds(half_poll_ms);

    // Daemon mode disables gate-time (always retry)
    if (cfg.run_as_daemon)
        cfg.gate_time = std::chrono::seconds{0};

    return cfg;
}

}  // namespace autosshpp
