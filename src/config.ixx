export module autosshpp.config;

import std;

export namespace autosshpp {

inline constexpr const char* VERSION = "1.0.0";
inline constexpr int DEFAULT_POLL_TIME = 600;
inline constexpr int DEFAULT_GATE_TIME = 30;
inline constexpr int DEFAULT_NET_TIMEOUT = 15000;
inline constexpr int MAX_CONN_TRIES = 3;
inline constexpr int N_FAST_TRIES = 5;
inline constexpr std::size_t MAX_MESSAGE_BYTES = 64;
inline constexpr std::string_view DETACHED_RELAUNCH_MARKER =
    "--autosshpp-detached";

enum class ControlCommand {
    none,
    stop,
    restart,
};

struct Config {
    std::uint16_t monitor_port = 0;
    std::uint16_t echo_port = 0;
    bool monitoring_enabled = true;

    std::chrono::seconds poll_time{DEFAULT_POLL_TIME};
    std::chrono::seconds first_poll_time{0};
    std::chrono::seconds gate_time{DEFAULT_GATE_TIME};
    std::chrono::milliseconds net_timeout{DEFAULT_NET_TIMEOUT};
    std::chrono::seconds max_lifetime{0};

    int max_starts = -1;

    std::string ssh_path = "ssh";
    std::string pid_file;
    std::string log_file;
    std::string message;
    int log_level = -1;
    bool debug = false;

    std::vector<std::string> ssh_args;

    bool run_as_daemon = false;
    bool detached_relaunch = false;
    ControlCommand control_command = ControlCommand::none;
    int control_pid = 0;

    [[nodiscard]] auto write_port() const -> std::uint16_t {
        return monitor_port;
    }

    [[nodiscard]] auto read_port() const -> std::uint16_t {
        return monitor_port + 1;
    }

    [[nodiscard]] auto effective_first_poll() const -> std::chrono::seconds {
        return first_poll_time > std::chrono::seconds{0}
            ? first_poll_time
            : poll_time;
    }
};

[[nodiscard]] auto parse_args(int argc, char* argv[]) -> Config;

}  // namespace autosshpp
