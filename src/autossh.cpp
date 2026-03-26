module;

#include <boost/asio.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#  include <boost/asio/windows/object_handle.hpp>
#  include <boost/process/v2/windows/creation_flags.hpp>
#  include <process.h>
#else
#  include <unistd.h>
#endif
#include <spdlog/spdlog.h>

module autosshpp.autossh;

import std;
import autosshpp.common;
import autosshpp.config;
import autosshpp.platform_control;

namespace autosshpp {

namespace asio = boost::asio;
namespace bp = boost::process;
using tcp = asio::ip::tcp;
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

using boost_error_code = boost::system::error_code;

namespace {

constexpr auto SSH_SHUTDOWN_GRACE_PERIOD = std::chrono::seconds{5};
constexpr auto SSH_SHUTDOWN_POLL_INTERVAL = std::chrono::milliseconds{100};

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

struct RestartPolicy {
    int start_count = 0;
    int fast_fail_count = 0;
    bool skip_backoff_once = false;
    std::chrono::steady_clock::time_point last_attempt_start{};

    [[nodiscard]] auto max_starts_reached(int max_starts) const -> bool {
        return max_starts >= 0 && start_count >= max_starts;
    }

    void note_requested_restart() {
        skip_backoff_once = true;
    }

    [[nodiscard]] auto begin_attempt() -> int {
        ++start_count;
        skip_backoff_once = false;
        last_attempt_start = std::chrono::steady_clock::now();
        return start_count;
    }

    void note_launch_failure() {
        ++fast_fail_count;
    }

    void note_connection_failure() {
        ++fast_fail_count;
    }

    [[nodiscard]] auto first_attempt() const -> bool {
        return start_count == 1;
    }

    [[nodiscard]] auto calculate_backoff(const Config& config)
        -> std::chrono::seconds
    {
        if (start_count == 0)
            return std::chrono::seconds{0};
        if (skip_backoff_once)
            return std::chrono::seconds{0};

        const auto uptime =
            std::chrono::steady_clock::now() - last_attempt_start;
        const auto min_time =
            std::max(config.poll_time / 10, std::chrono::seconds{10});

        if (uptime >= min_time) {
            fast_fail_count = 0;
            return std::chrono::seconds{0};
        }

        if (fast_fail_count <= N_FAST_TRIES)
            return std::chrono::seconds{0};

        const auto n = fast_fail_count - N_FAST_TRIES;
        const auto delay_sec =
            static_cast<std::int64_t>(n) * n / 3 * config.poll_time.count() / 100;
        const auto delay = std::chrono::seconds(
            std::max(delay_sec, static_cast<std::chrono::seconds::rep>(1)));
        return std::min(delay, config.poll_time);
    }

    [[nodiscard]] auto classify_exit(int exit_code,
                                     const Config& config,
                                     std::chrono::steady_clock::time_point ssh_start)
        -> RestartAction
    {
        const auto uptime = std::chrono::steady_clock::now() - ssh_start;

        if (first_attempt() && config.gate_time > std::chrono::seconds{0} &&
            uptime <= config.gate_time)
        {
            spdlog::error(
                "ssh exited prematurely with status {} within gate time",
                exit_code);
            return RestartAction::exit_error;
        }

        switch (exit_code) {
        case 255:
            ++fast_fail_count;
            return RestartAction::restart;
        case 2:
        case 1:
            if (!first_attempt() || config.gate_time == std::chrono::seconds{0}) {
                ++fast_fail_count;
                return RestartAction::restart;
            }
            spdlog::info("ssh exited with error status {}; autossh++ exiting",
                         exit_code);
            return RestartAction::exit_error;
        case 0:
            spdlog::info("ssh exited normally, not restarting");
            return RestartAction::exit_ok;
        default:
            spdlog::info("ssh exited with status {}; autossh++ exiting",
                         exit_code);
            return RestartAction::exit_error;
        }
    }
};

struct ShutdownState {
    RequestedAction requested_action = RequestedAction::none;
    SshShutdownStage stage = SshShutdownStage::none;
    std::uint64_t generation = 0;

    [[nodiscard]] auto request(RequestedAction action,
                               RestartPolicy& restart_policy) -> bool
    {
        if (requested_action == RequestedAction::stop)
            return false;

        if (action == RequestedAction::stop) {
            requested_action = RequestedAction::stop;
            return true;
        }

        if (action == RequestedAction::restart) {
            if (requested_action == RequestedAction::restart)
                return false;
            requested_action = RequestedAction::restart;
            restart_policy.note_requested_restart();
            return true;
        }

        return false;
    }

    [[nodiscard]] auto stop_requested() const -> bool {
        return requested_action == RequestedAction::stop;
    }

    [[nodiscard]] auto action_pending() const -> bool {
        return requested_action != RequestedAction::none;
    }

    [[nodiscard]] auto consume_restart_request() -> bool {
        if (requested_action != RequestedAction::restart)
            return false;

        requested_action = RequestedAction::none;
        return true;
    }

    [[nodiscard]] auto begin_graceful_shutdown() -> std::uint64_t {
        stage = SshShutdownStage::graceful;
        return ++generation;
    }

    void mark_forced_shutdown() {
        stage = SshShutdownStage::forced;
        ++generation;
    }

    void clear_shutdown_stage() {
        stage = SshShutdownStage::none;
        ++generation;
    }
};

struct RuntimeState {
    int exit_code = 0;
    bool immediate_exit_requested = false;
    std::optional<Error> fatal_error;
    std::chrono::steady_clock::time_point daemon_start{};
    std::chrono::steady_clock::time_point ssh_start{};
};

struct SessionOutcome {
    int exit_code = 0;
    bool connection_failed = false;
    bool lifetime_reached = false;
};

[[nodiscard]] auto make_boost_error(ErrorCode code,
                                    std::string message,
                                    const boost_error_code& error) -> Error
{
    return make_error(
        code,
        std::format("{}: {}", std::move(message), error.message()));
}

[[nodiscard]] auto make_std_error(ErrorCode code,
                                  std::string message,
                                  const std::error_code& error) -> Error
{
    return make_error(
        code,
        std::format("{}: {}", std::move(message), error.message()));
}

struct MonitorSession {
    std::optional<Acceptor> acceptor;
    std::string test_message;

    explicit MonitorSession(const Config& config) {
        auto hostname = asio::ip::host_name();
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        std::random_device rd;
        test_message = std::format("{} autossh {} {:08x}", hostname, pid, rd());
        if (!config.message.empty()) {
            test_message += ' ';
            test_message += config.message;
        }
        test_message += "\r\n";
    }

    [[nodiscard]] auto setup_listener(asio::io_context& io, const Config& config)
        -> Result<void>
    {
        if (!config.monitoring_enabled || config.echo_port != 0)
            return {};

        const auto endpoint = tcp::endpoint(
            asio::ip::make_address("127.0.0.1"),
            config.read_port());

        Acceptor listener(io);
        boost_error_code ec;

        listener.open(endpoint.protocol(), ec);
        if (ec) {
            return std::unexpected(make_boost_error(
                ErrorCode::io_failure,
                std::format("failed to open monitor listener on 127.0.0.1:{}",
                            config.read_port()),
                ec));
        }

        listener.set_option(tcp::acceptor::reuse_address(true), ec);
        if (ec) {
            return std::unexpected(make_boost_error(
                ErrorCode::io_failure,
                "failed to configure monitor listener socket options",
                ec));
        }

        listener.bind(endpoint, ec);
        if (ec) {
            return std::unexpected(make_boost_error(
                ErrorCode::io_failure,
                std::format("failed to bind monitor listener on 127.0.0.1:{}",
                            config.read_port()),
                ec));
        }

        listener.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            return std::unexpected(make_boost_error(
                ErrorCode::io_failure,
                "failed to listen on monitor listener socket",
                ec));
        }

        acceptor.emplace(std::move(listener));
        spdlog::info("monitor: write port {}, read port {}",
                     config.write_port(), config.read_port());
        return {};
    }

    void cancel_pending() {
        if (!acceptor)
            return;

        boost_error_code ec;
        acceptor->cancel(ec);
    }

    [[nodiscard]] auto build_ssh_args(const Config& config) const
        -> std::vector<std::string>
    {
        std::vector<std::string> args;

        if (config.monitoring_enabled) {
            args.push_back("-L");
            if (config.echo_port != 0) {
                args.push_back(std::format(
                    "{}:127.0.0.1:{}",
                    config.write_port(),
                    config.echo_port));
            } else {
                args.push_back(std::format(
                    "{}:127.0.0.1:{}",
                    config.write_port(),
                    config.write_port()));
                args.push_back("-R");
                args.push_back(std::format(
                    "{}:127.0.0.1:{}",
                    config.write_port(),
                    config.read_port()));
            }
        }

        args.insert(args.end(), config.ssh_args.begin(), config.ssh_args.end());
        return args;
    }
};

}  // namespace

struct AutoSSH::Impl {
    explicit Impl(Config config)
        : config_(std::move(config))
        , monitor_(config_)
        , backoff_timer_(io_)
        , loop_wait_timer_(io_)
        , shutdown_timer_(io_)
        , shutdown_poll_timer_(io_)
        , signals_(io_)
    {
    }

    ~Impl() {
        remove_pid_file();
    }

    [[nodiscard]] auto run() -> Result<int> {
        setup_signals();

        if (auto platform_control = setup_platform_control(); !platform_control)
            return std::unexpected(platform_control.error());

        if (auto monitor_setup = monitor_.setup_listener(io_, config_);
            !monitor_setup)
        {
            return std::unexpected(monitor_setup.error());
        }

        if (auto pid_write = write_pid_file(); !pid_write)
            return std::unexpected(pid_write.error());

        asio::co_spawn(io_, main_loop(), [this](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    runtime_.fatal_error = make_error(
                        ErrorCode::internal_failure,
                        std::format("autossh main loop failed: {}", e.what()));
                    runtime_.exit_code = 1;
                }
            }
            io_.stop();
        });

        io_.run();

        if (runtime_.fatal_error)
            return std::unexpected(*runtime_.fatal_error);

        return runtime_.exit_code;
    }

    void setup_signals() {
        for (const auto signal_number : platform_control_signals())
            signals_.add(signal_number);

        arm_signal_wait();
    }

    void arm_signal_wait() {
        signals_.async_wait([this](boost_error_code ec, int sig) {
            if (ec)
                return;

            const auto action = requested_action_for_signal(sig);
            switch (action) {
            case RequestedAction::restart:
                spdlog::info("received signal {}, restart requested", sig);
                request_action(RequestedAction::restart);
                arm_signal_wait();
                return;
            case RequestedAction::stop:
                spdlog::info("received signal {}, shutting down", sig);
                request_action(RequestedAction::stop);
                arm_force_exit_wait();
                return;
            case RequestedAction::none:
                arm_signal_wait();
                return;
            }
        });
    }

    void arm_force_exit_wait() {
        signals_.async_wait([this](boost_error_code ec, int) {
            if (ec)
                return;

            force_terminate_ssh();
            request_immediate_exit(1);
        });
    }

    [[nodiscard]] auto setup_platform_control() -> Result<void> {
#ifdef _WIN32
        const auto pid = static_cast<DWORD>(_getpid());

        auto restart_event = create_control_event(RequestedAction::restart, pid);
        if (!restart_event)
            return std::unexpected(restart_event.error());

        auto stop_event = create_control_event(RequestedAction::stop, pid);
        if (!stop_event)
            return std::unexpected(stop_event.error());

        restart_control_event_.emplace(io_, restart_event->release());
        stop_control_event_.emplace(io_, stop_event->release());

        arm_platform_control_wait(*restart_control_event_, RequestedAction::restart);
        arm_platform_control_wait(*stop_control_event_, RequestedAction::stop);
#endif

        return {};
    }

#ifdef _WIN32
    void arm_platform_control_wait(ObjectHandle& handle, RequestedAction action) {
        handle.async_wait([this, &handle, action](boost_error_code ec) {
            if (ec)
                return;

            switch (action) {
            case RequestedAction::restart:
                spdlog::info("received Windows control restart request");
                request_action(RequestedAction::restart);
                arm_platform_control_wait(handle, action);
                return;
            case RequestedAction::stop:
                if (shutdown_.stop_requested()) {
                    spdlog::warn(
                        "received second Windows control stop request, forcing exit");
                    force_terminate_ssh();
                    request_immediate_exit(1);
                    return;
                }
                spdlog::info("received Windows control stop request");
                request_action(RequestedAction::stop);
                arm_platform_control_wait(handle, action);
                return;
            case RequestedAction::none:
                arm_platform_control_wait(handle, action);
                return;
            }
        });
    }
#endif

    void request_action(RequestedAction action) {
        if (!shutdown_.request(action, restart_policy_))
            return;

        backoff_timer_.cancel();
        loop_wait_timer_.cancel();
        monitor_.cancel_pending();
        begin_ssh_shutdown();
    }

    void request_immediate_exit(int exit_code) {
        runtime_.immediate_exit_requested = true;
        runtime_.exit_code = exit_code;
        backoff_timer_.cancel();
        loop_wait_timer_.cancel();
        shutdown_timer_.cancel();
        shutdown_poll_timer_.cancel();
        monitor_.cancel_pending();
        io_.stop();
    }

    [[nodiscard]] auto stop_requested() const -> bool {
        return shutdown_.stop_requested() || runtime_.immediate_exit_requested;
    }

    [[nodiscard]] auto start_ssh() -> Result<void> {
        auto args = monitor_.build_ssh_args(config_);
        const auto attempt_number = restart_policy_.begin_attempt();

        spdlog::info("starting ssh (attempt {})", attempt_number);
        if (spdlog::should_log(spdlog::level::debug)) {
            std::string cmd = config_.ssh_path;
            for (const auto& arg : args) {
                cmd += ' ';
                cmd += arg;
            }
            spdlog::debug("command: {}", cmd);
        }

        try {
            std::filesystem::path exe;
            if (config_.ssh_path.find_first_of("/\\") != std::string::npos) {
                exe = config_.ssh_path;
            } else {
                exe = bp::environment::find_executable(config_.ssh_path);
                if (exe.empty()) {
                    return std::unexpected(make_error(
                        ErrorCode::process_failure,
                        std::format("'{}' not found in PATH", config_.ssh_path)));
                }
            }

#ifdef _WIN32
            ssh_.emplace(io_.get_executor(),
                         exe,
                         args,
                         bp::windows::create_new_process_group);
#else
            ssh_.emplace(io_.get_executor(), exe, args);
#endif

            clear_ssh_shutdown_state();
            runtime_.ssh_start = restart_policy_.last_attempt_start;
            return {};
        } catch (const std::exception& e) {
            ssh_.reset();
            return std::unexpected(make_error(
                ErrorCode::process_failure,
                std::format("failed to start ssh: {}", e.what())));
        }
    }

    void begin_ssh_shutdown() {
        if (!ssh_)
            return;

        boost_error_code ec;
        if (!ssh_->running(ec))
            return;
        if (ec) {
            spdlog::warn("failed to query ssh process state before shutdown: {}",
                         ec.message());
            return;
        }

        if (shutdown_.stage != SshShutdownStage::none)
            return;

#ifndef _WIN32
        ssh_->request_exit(ec);
        if (!ec) {
            const auto generation = shutdown_.begin_graceful_shutdown();
            spdlog::info("requested graceful ssh shutdown");
            arm_ssh_shutdown_timer(generation);
            return;
        }

        boost_error_code still_running_ec;
        if (!ssh_->running(still_running_ec))
            return;

        spdlog::warn("failed to request graceful ssh shutdown: {}", ec.message());
#else
        auto graceful_shutdown =
            request_process_group_interrupt(static_cast<DWORD>(ssh_->id()));
        if (graceful_shutdown) {
            const auto generation = shutdown_.begin_graceful_shutdown();
            spdlog::info("requested graceful ssh shutdown");
            arm_ssh_shutdown_timer(generation);
            return;
        }

        spdlog::warn("failed to request graceful ssh shutdown: {}",
                     graceful_shutdown.error().message);
#endif

        force_terminate_ssh();
    }

    void arm_ssh_shutdown_timer(std::uint64_t shutdown_generation) {
        shutdown_timer_.expires_after(SSH_SHUTDOWN_GRACE_PERIOD);
        shutdown_timer_.async_wait(
            [this, shutdown_generation](boost_error_code ec) {
                if (ec || shutdown_generation != shutdown_.generation ||
                    shutdown_.stage != SshShutdownStage::graceful)
                {
                    return;
                }

                if (!ssh_)
                    return;

                boost_error_code running_ec;
                if (!ssh_->running(running_ec))
                    return;

                if (running_ec) {
                    spdlog::warn(
                        "failed while waiting for graceful ssh shutdown: {}",
                        running_ec.message());
                } else {
                    spdlog::warn(
                        "ssh did not exit after {}s grace period, forcing termination",
                        SSH_SHUTDOWN_GRACE_PERIOD.count());
                }

                force_terminate_ssh();
            });
    }

    void force_terminate_ssh() {
        if (!ssh_)
            return;

        boost_error_code ec;
        if (!ssh_->running(ec))
            return;
        if (ec) {
            spdlog::warn("failed to query ssh process state before termination: {}",
                         ec.message());
            return;
        }

        ssh_->terminate(ec);
        if (ec) {
            spdlog::warn("failed to terminate ssh process: {}", ec.message());
            return;
        }

        shutdown_.mark_forced_shutdown();
        spdlog::info("terminated ssh process");
    }

    void clear_ssh_shutdown_state() {
        shutdown_.clear_shutdown_stage();
        shutdown_timer_.cancel();
        shutdown_poll_timer_.cancel();
    }

    [[nodiscard]] auto ssh_running() -> bool {
        if (!ssh_)
            return false;

        try {
            boost_error_code ec;
            return ssh_->running(ec);
        } catch (...) {
            return false;
        }
    }

    auto wait_for_ssh_shutdown() -> IoAwaitable<void> {
        if (!ssh_) {
            clear_ssh_shutdown_state();
            co_return;
        }

        begin_ssh_shutdown();

        while (ssh_running() && !runtime_.immediate_exit_requested) {
            shutdown_poll_timer_.expires_after(SSH_SHUTDOWN_POLL_INTERVAL);
            co_await shutdown_poll_timer_.async_wait(
                asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
        }

        clear_ssh_shutdown_state();
    }

    auto run_session() -> IoAwaitable<SessionOutcome> {
        SessionOutcome outcome;
        auto poll_delay = restart_policy_.first_attempt()
            ? config_.effective_first_poll()
            : config_.poll_time;

        while (ssh_running() && !stop_requested()) {
            if (config_.max_lifetime > std::chrono::seconds{0}) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - runtime_.daemon_start);
                const auto remaining = config_.max_lifetime - elapsed;
                if (remaining <= std::chrono::seconds{0}) {
                    outcome.lifetime_reached = true;
                    begin_ssh_shutdown();
                    break;
                }
                poll_delay = std::min(poll_delay, remaining);
            }

            const auto still_up = co_await wait_or_ssh_exit(poll_delay);
            if (!still_up || stop_requested())
                break;

            if (config_.monitoring_enabled) {
                spdlog::debug("testing connection...");
                const auto alive = co_await test_connection();
                if (!alive && ssh_running()) {
                    spdlog::warn("connection test failed, restarting ssh");
                    begin_ssh_shutdown();
                    outcome.connection_failed = true;
                    break;
                }
                spdlog::debug("connection ok");
            }

            poll_delay = config_.poll_time;
        }

        if (ssh_ && (outcome.connection_failed || outcome.lifetime_reached ||
                     shutdown_.action_pending() || stop_requested()))
        {
            co_await wait_for_ssh_shutdown();
        }

        if (outcome.connection_failed) {
            outcome.exit_code = 255;
        } else if (ssh_) {
            boost_error_code ec;
            if (!ssh_->running(ec))
                outcome.exit_code = ssh_->exit_code();
        }

        co_return outcome;
    }

    auto main_loop() -> IoAwaitable<void> {
        runtime_.daemon_start = std::chrono::steady_clock::now();

        spdlog::info("autossh++ {} starting", VERSION);
        if (!config_.monitoring_enabled)
            spdlog::info("monitoring disabled (-M 0), relying on SSH ServerAlive");

        while (!stop_requested()) {
            if (restart_policy_.max_starts_reached(config_.max_starts)) {
                spdlog::info("max starts ({}) reached", config_.max_starts);
                break;
            }
            if (!check_lifetime())
                break;

            const auto backoff = restart_policy_.calculate_backoff(config_);
            if (backoff > std::chrono::seconds{0}) {
                spdlog::info("backoff: waiting {}s before restart", backoff.count());
                backoff_timer_.expires_after(backoff);
                co_await backoff_timer_.async_wait(
                    asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
                if (stop_requested())
                    break;
            }

            if (shutdown_.consume_restart_request())
                spdlog::info("processing requested restart");

            auto start_result = start_ssh();
            if (!start_result) {
                restart_policy_.note_launch_failure();
                runtime_.fatal_error = start_result.error();
                runtime_.exit_code = 1;
                break;
            }

            auto session = co_await run_session();
            const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - runtime_.ssh_start);
            spdlog::info("ssh exited (code {}), uptime: {}s",
                         session.exit_code, uptime.count());

            ssh_.reset();
            clear_ssh_shutdown_state();

            if (stop_requested())
                break;
            if (shutdown_.consume_restart_request()) {
                spdlog::info("processing requested restart");
                continue;
            }
            if (session.lifetime_reached) {
                runtime_.exit_code = 0;
                break;
            }
            if (session.connection_failed) {
                restart_policy_.note_connection_failure();
                continue;
            }

            switch (restart_policy_.classify_exit(
                        session.exit_code, config_, runtime_.ssh_start))
            {
            case RestartAction::restart:
                continue;
            case RestartAction::exit_ok:
                runtime_.exit_code = 0;
                break;
            case RestartAction::exit_error:
                runtime_.exit_code = 1;
                break;
            }

            break;
        }

        co_await wait_for_ssh_shutdown();
        spdlog::info("autossh++ exiting");
    }

    auto wait_or_ssh_exit(std::chrono::seconds duration) -> IoAwaitable<bool> {
        const auto deadline = std::chrono::steady_clock::now() + duration;

        while (std::chrono::steady_clock::now() < deadline) {
            if (!ssh_running() || stop_requested())
                co_return false;

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            const auto wait = std::min(remaining, std::chrono::milliseconds{2000});
            if (wait <= std::chrono::milliseconds{0})
                break;

            loop_wait_timer_.expires_after(wait);
            co_await loop_wait_timer_.async_wait(
                asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
        }

        co_return ssh_running() && !stop_requested();
    }

    auto test_connection() -> IoAwaitable<bool> {
        for (int attempt = 0; attempt < MAX_CONN_TRIES; ++attempt) {
            if (co_await test_connection_once())
                co_return true;
            if (stop_requested() || !ssh_running())
                co_return false;
            spdlog::debug("connection test attempt {}/{} failed",
                          attempt + 1, MAX_CONN_TRIES);
        }

        co_return false;
    }

    auto test_connection_once() -> IoAwaitable<bool> {
        const auto executor = co_await asio::this_coro::executor;

        Timer timeout(executor);
        timeout.expires_after(config_.net_timeout);

        Socket write_sock(executor);
        Socket read_sock(executor);

        auto cancel = [&] {
            boost_error_code ec;
            write_sock.close(ec);
            read_sock.close(ec);
        };

        timeout.async_wait([this, &cancel](boost_error_code ec) {
            if (!ec) {
                cancel();
                monitor_.cancel_pending();
            }
        });

        const auto write_endpoint = tcp::endpoint(
            asio::ip::make_address("127.0.0.1"),
            config_.write_port());
        auto [connect_ec] = co_await write_sock.async_connect(
            write_endpoint, asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
        if (connect_ec) {
            timeout.cancel();
            co_return false;
        }

        if (config_.echo_port == 0 && monitor_.acceptor) {
            auto [accept_ec] = co_await monitor_.acceptor->async_accept(
                read_sock, asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
            if (accept_ec) {
                timeout.cancel();
                cancel();
                co_return false;
            }
        }

        auto [write_ec, written] = co_await asio::async_write(
            write_sock,
            asio::buffer(monitor_.test_message),
            asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));
        if (write_ec) {
            timeout.cancel();
            cancel();
            co_return false;
        }

        (void)written;

        auto& recv = (config_.echo_port != 0) ? write_sock : read_sock;
        std::string response(monitor_.test_message.size(), '\0');
        auto [read_ec, read_bytes] = co_await asio::async_read(
            recv,
            asio::buffer(response),
            asio::as_tuple(asio::use_awaitable_t<IoExecutor>{}));

        timeout.cancel();
        cancel();

        (void)read_bytes;

        if (read_ec)
            co_return false;
        co_return response == monitor_.test_message;
    }

    [[nodiscard]] auto check_lifetime() -> bool {
        if (config_.max_lifetime <= std::chrono::seconds{0})
            return true;

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - runtime_.daemon_start);
        if (elapsed >= config_.max_lifetime) {
            spdlog::info("max lifetime ({}s) reached", config_.max_lifetime.count());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto write_pid_file() -> Result<void> {
        if (config_.pid_file.empty())
            return {};

        std::ofstream file(config_.pid_file);
        if (!file) {
            return std::unexpected(make_error(
                ErrorCode::pid_file_failure,
                std::format("cannot open pid file: {}", config_.pid_file)));
        }

#ifdef _WIN32
        file << _getpid() << '\n';
#else
        file << getpid() << '\n';
#endif
        if (!file) {
            return std::unexpected(make_error(
                ErrorCode::pid_file_failure,
                std::format("cannot write pid file: {}", config_.pid_file)));
        }

        pid_file_created_ = true;
        return {};
    }

    void remove_pid_file() {
        if (!pid_file_created_ || config_.pid_file.empty())
            return;

        std::error_code ec;
        std::filesystem::remove(config_.pid_file, ec);
        pid_file_created_ = false;
    }

    asio::io_context io_;
    Config config_;
    MonitorSession monitor_;

    std::optional<Process> ssh_;
    Timer backoff_timer_;
    Timer loop_wait_timer_;
    Timer shutdown_timer_;
    Timer shutdown_poll_timer_;
    SignalSet signals_;
#ifdef _WIN32
    std::optional<ObjectHandle> restart_control_event_;
    std::optional<ObjectHandle> stop_control_event_;
#endif

    RestartPolicy restart_policy_;
    ShutdownState shutdown_;
    RuntimeState runtime_;
    bool pid_file_created_ = false;
};

AutoSSH::AutoSSH(Config config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

AutoSSH::~AutoSSH() = default;

AutoSSH::AutoSSH(AutoSSH&&) noexcept = default;

auto AutoSSH::operator=(AutoSSH&&) noexcept -> AutoSSH& = default;

auto AutoSSH::run() -> Result<int> {
    return impl_->run();
}

}  // namespace autosshpp
