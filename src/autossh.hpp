#pragma once

#include <boost/asio.hpp>
#include <boost/process.hpp>

#include "config.hpp"
#include "platform_control.hpp"

namespace autosshpp {

namespace asio = boost::asio;
namespace bp   = boost::process;
using tcp      = asio::ip::tcp;

class AutoSSH {
public:
    explicit AutoSSH(asio::io_context& io, Config config);
    ~AutoSSH();

    void run();

    [[nodiscard]] int exit_code() const { return exit_code_; }

private:
    enum class RestartAction {
        restart,
        exit_ok,
        exit_error,
    };

    void setup_signals();
    void arm_signal_wait();
    void arm_force_exit_wait();
    void setup_monitor_listener();
    void request_action(RequestedAction action);
    [[nodiscard]] bool stop_requested() const;
    [[nodiscard]] bool consume_restart_request();

    std::vector<std::string> build_ssh_args();
    [[nodiscard]] bool start_ssh();
    void kill_ssh();
    bool ssh_running();

    asio::awaitable<void> main_loop();
    asio::awaitable<bool> wait_or_ssh_exit(std::chrono::seconds duration);
    asio::awaitable<bool> test_connection();
    asio::awaitable<bool> test_connection_once();

    RestartAction classify_exit(int exit_code, bool first_attempt);
    std::chrono::seconds calculate_backoff();
    bool check_lifetime();

    void write_pid_file();
    void remove_pid_file();

    // ── Members ─────────────────────────────────────────────────────
    asio::io_context& io_;
    Config config_;

    std::optional<bp::process> ssh_;
    std::optional<tcp::acceptor> monitor_acceptor_;
    asio::steady_timer poll_timer_;
    asio::signal_set signals_;

    int  start_count_      = 0;
    int  fast_fail_count_  = 0;
    int  exit_code_        = 0;
    bool skip_backoff_once_ = false;
    RequestedAction requested_action_ = RequestedAction::none;

    std::chrono::steady_clock::time_point last_attempt_start_;
    std::chrono::steady_clock::time_point daemon_start_;
    std::chrono::steady_clock::time_point ssh_start_;

    std::string test_message_;
};

}  // namespace autosshpp
