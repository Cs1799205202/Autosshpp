module;

#include <boost/asio.hpp>
#include <boost/process.hpp>

#ifdef _WIN32
#  include <boost/asio/windows/object_handle.hpp>
#endif

export module autosshpp.autossh;

import std;
import autosshpp.config;
import autosshpp.platform_control;

export namespace autosshpp {

namespace asio = boost::asio;
namespace bp   = boost::process;
using tcp      = asio::ip::tcp;
using IoExecutor = asio::io_context::executor_type;
using Process = bp::basic_process<IoExecutor>;
using Timer = asio::basic_waitable_timer<std::chrono::steady_clock,
                                         asio::wait_traits<std::chrono::steady_clock>,
                                         IoExecutor>;
using Socket = asio::basic_stream_socket<tcp, IoExecutor>;
using Acceptor = asio::basic_socket_acceptor<tcp, IoExecutor>;
using SignalSet = asio::basic_signal_set<IoExecutor>;

template <typename T>
using IoAwaitable = asio::awaitable<T, IoExecutor>;

#ifdef _WIN32
using ObjectHandle = asio::windows::basic_object_handle<IoExecutor>;
#endif

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
    enum class SshShutdownStage {
        none,
        graceful,
        forced,
    };

    void setup_signals();
    void arm_signal_wait();
    void arm_force_exit_wait();
    [[nodiscard]] auto setup_platform_control() -> std::expected<void, std::string>;
#ifdef _WIN32
    void arm_platform_control_wait(ObjectHandle& handle, RequestedAction action);
#endif
    void setup_monitor_listener();
    void request_action(RequestedAction action);
    [[nodiscard]] bool stop_requested() const;
    [[nodiscard]] bool consume_restart_request();

    std::vector<std::string> build_ssh_args();
    [[nodiscard]] bool start_ssh();
    void begin_ssh_shutdown();
    void arm_ssh_shutdown_timer(std::uint64_t shutdown_generation);
    void force_terminate_ssh();
    void clear_ssh_shutdown_state();
    bool ssh_running();
    IoAwaitable<void> wait_for_ssh_shutdown();

    IoAwaitable<void> main_loop();
    IoAwaitable<bool> wait_or_ssh_exit(std::chrono::seconds duration);
    IoAwaitable<bool> test_connection();
    IoAwaitable<bool> test_connection_once();

    RestartAction classify_exit(int exit_code, bool first_attempt);
    std::chrono::seconds calculate_backoff();
    bool check_lifetime();

    void write_pid_file();
    void remove_pid_file();

    asio::io_context& io_;
    Config config_;

    std::optional<Process> ssh_;
    std::optional<Acceptor> monitor_acceptor_;
    Timer poll_timer_;
    Timer shutdown_timer_;
    SignalSet signals_;
#ifdef _WIN32
    std::optional<ObjectHandle> restart_control_event_;
    std::optional<ObjectHandle> stop_control_event_;
#endif

    int  start_count_      = 0;
    int  fast_fail_count_  = 0;
    int  exit_code_        = 0;
    bool skip_backoff_once_ = false;
    RequestedAction requested_action_ = RequestedAction::none;
    SshShutdownStage ssh_shutdown_stage_ = SshShutdownStage::none;
    std::uint64_t ssh_shutdown_generation_ = 0;

    std::chrono::steady_clock::time_point last_attempt_start_;
    std::chrono::steady_clock::time_point daemon_start_;
    std::chrono::steady_clock::time_point ssh_start_;

    std::string test_message_;
};

}  // namespace autosshpp
