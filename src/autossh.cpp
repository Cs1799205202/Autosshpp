#include "autossh.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

// #include <algorithm>
// #include <exception>
// #include <filesystem>
// #include <format>
// #include <fstream>
// #include <random>

#ifdef _WIN32
#  include <process.h>   // _getpid
#else
#  include <unistd.h>    // getpid
#endif

import std;

namespace autosshpp {

using error_code = boost::system::error_code;

// ════════════════════════════════════════════════════════════════════
//  Construction / destruction
// ════════════════════════════════════════════════════════════════════

AutoSSH::AutoSSH(asio::io_context& io, Config config)
    : io_(io)
    , config_(std::move(config))
    , poll_timer_(io)
    , signals_(io)
{
    auto hostname = asio::ip::host_name();
#ifdef _WIN32
    auto pid = _getpid();
#else
    auto pid = getpid();
#endif
    std::random_device rd;
    test_message_ = std::format("{} autossh {} {:08x}", hostname, pid, rd());
    if (!config_.message.empty()) {
        test_message_ += ' ';
        test_message_ += config_.message;
    }
    test_message_ += "\r\n";
}

AutoSSH::~AutoSSH() {
    remove_pid_file();
}

// ════════════════════════════════════════════════════════════════════
//  Public entry point
// ════════════════════════════════════════════════════════════════════

void AutoSSH::run() {
    setup_signals();
    if (auto platform_control = setup_platform_control(); !platform_control) {
        spdlog::error("fatal: {}", platform_control.error());
        exit_code_ = 1;
        return;
    }

    if (config_.monitoring_enabled && config_.echo_port == 0)
        setup_monitor_listener();

    write_pid_file();

    asio::co_spawn(io_, main_loop(), [this](std::exception_ptr ep) {
        if (ep) {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) {
                spdlog::error("fatal: {}", e.what());
                exit_code_ = 1;
            }
        }
        io_.stop();
    });

    io_.run();
}

// ════════════════════════════════════════════════════════════════════
//  Setup helpers
// ════════════════════════════════════════════════════════════════════

void AutoSSH::setup_signals() {
    for (const auto signal_number : platform_control_signals())
        signals_.add(signal_number);

    arm_signal_wait();
}

void AutoSSH::arm_signal_wait() {
    signals_.async_wait([this](error_code ec, int sig) {
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

void AutoSSH::arm_force_exit_wait() {
    signals_.async_wait([](error_code ec, int) {
        if (!ec)
            std::exit(1);
    });
}

auto AutoSSH::setup_platform_control() -> std::expected<void, std::string> {
#ifdef _WIN32
    const auto pid = static_cast<DWORD>(_getpid());

    auto restart_event = create_control_event(RequestedAction::restart, pid);
    if (!restart_event)
        return std::unexpected(restart_event.error());

    auto stop_event = create_control_event(RequestedAction::stop, pid);
    if (!stop_event) {
        ::CloseHandle(*restart_event);
        return std::unexpected(stop_event.error());
    }

    restart_control_event_.emplace(io_, *restart_event);
    stop_control_event_.emplace(io_, *stop_event);

    arm_platform_control_wait(*restart_control_event_, RequestedAction::restart);
    arm_platform_control_wait(*stop_control_event_, RequestedAction::stop);
#endif

    return {};
}

#ifdef _WIN32
void AutoSSH::arm_platform_control_wait(asio::windows::object_handle& handle,
                                        RequestedAction action) {
    handle.async_wait([this, &handle, action](error_code ec) {
        if (ec)
            return;

        switch (action) {
        case RequestedAction::restart:
            spdlog::info("received Windows control restart request");
            request_action(RequestedAction::restart);
            arm_platform_control_wait(handle, action);
            return;
        case RequestedAction::stop:
            if (stop_requested()) {
                spdlog::warn(
                    "received second Windows control stop request, forcing exit");
                std::exit(1);
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

void AutoSSH::setup_monitor_listener() {
    auto ep = tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                            config_.read_port());
    monitor_acceptor_.emplace(io_, ep);
    error_code ec;
    monitor_acceptor_->set_option(tcp::acceptor::reuse_address(true), ec);

    spdlog::info("monitor: write port {}, read port {}",
                 config_.write_port(), config_.read_port());
}

void AutoSSH::request_action(RequestedAction action) {
    if (requested_action_ == RequestedAction::stop)
        return;

    if (action == RequestedAction::stop) {
        requested_action_ = RequestedAction::stop;
    } else if (action == RequestedAction::restart) {
        if (requested_action_ == RequestedAction::restart)
            return;
        requested_action_ = RequestedAction::restart;
        skip_backoff_once_ = true;
    } else {
        return;
    }

    poll_timer_.cancel();
    if (monitor_acceptor_) {
        error_code ec;
        monitor_acceptor_->cancel(ec);
    }
    kill_ssh();
}

bool AutoSSH::stop_requested() const {
    return requested_action_ == RequestedAction::stop;
}

bool AutoSSH::consume_restart_request() {
    if (requested_action_ != RequestedAction::restart)
        return false;

    requested_action_ = RequestedAction::none;
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  SSH process management
// ════════════════════════════════════════════════════════════════════

std::vector<std::string> AutoSSH::build_ssh_args() {
    std::vector<std::string> args;

    if (config_.monitoring_enabled) {
        args.push_back("-L");
        if (config_.echo_port != 0) {
            args.push_back(std::format("{}:127.0.0.1:{}",
                config_.write_port(), config_.echo_port));
        } else {
            args.push_back(std::format("{}:127.0.0.1:{}",
                config_.write_port(), config_.write_port()));
            args.push_back("-R");
            args.push_back(std::format("{}:127.0.0.1:{}",
                config_.write_port(), config_.read_port()));
        }
    }

    args.insert(args.end(), config_.ssh_args.begin(), config_.ssh_args.end());
    return args;
}

bool AutoSSH::start_ssh() {
    auto args = build_ssh_args();
    const auto attempt_number = start_count_ + 1;
    start_count_ = attempt_number;
    skip_backoff_once_ = false;
    last_attempt_start_ = std::chrono::steady_clock::now();

    spdlog::info("starting ssh (attempt {})", attempt_number);
    if (spdlog::should_log(spdlog::level::debug)) {
        std::string cmd = config_.ssh_path;
        for (auto& a : args) { cmd += ' '; cmd += a; }
        spdlog::debug("command: {}", cmd);
    }

    try {
        // Resolve executable path
        std::filesystem::path exe;
        if (config_.ssh_path.find_first_of("/\\") != std::string::npos) {
            exe = config_.ssh_path;
        } else {
            exe = bp::environment::find_executable(config_.ssh_path);
            if (exe.empty()) {
                spdlog::error("'{}' not found in PATH", config_.ssh_path);
                return false;
            }
        }

        // Boost.Process v2: process(executor, path, args)
        ssh_.emplace(io_.get_executor(), exe, args);
        ssh_start_ = last_attempt_start_;
        return true;
    } catch (const std::exception& e) {
        spdlog::error("failed to start ssh: {}", e.what());
        ssh_.reset();
        return false;
    }
}

void AutoSSH::kill_ssh() {
    if (!ssh_) return;
    try {
        error_code ec;
        if (ssh_->running(ec)) {
            ssh_->terminate(ec);
            ssh_->wait(ec);
        }
    } catch (...) {}
}

bool AutoSSH::ssh_running() {
    if (!ssh_) return false;
    try {
        error_code ec;
        return ssh_->running(ec);
    } catch (...) {
        return false;
    }
}

// ════════════════════════════════════════════════════════════════════
//  Main coroutine loop
// ════════════════════════════════════════════════════════════════════

asio::awaitable<void> AutoSSH::main_loop() {
    daemon_start_ = std::chrono::steady_clock::now();

    spdlog::info("autossh++ {} starting", VERSION);
    if (!config_.monitoring_enabled)
        spdlog::info("monitoring disabled (-M 0), relying on SSH ServerAlive");

    while (!stop_requested()) {
        if (config_.max_starts >= 0 && start_count_ >= config_.max_starts) {
            spdlog::info("max starts ({}) reached", config_.max_starts);
            break;
        }
        if (!check_lifetime()) break;

        auto backoff = calculate_backoff();
        if (backoff > std::chrono::seconds{0}) {
            spdlog::info("backoff: waiting {}s before restart", backoff.count());
            poll_timer_.expires_after(backoff);
            auto [ec] = co_await poll_timer_.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (stop_requested())
                break;
        }

        if (consume_restart_request())
            spdlog::info("processing requested restart");

        if (!start_ssh()) {
            fast_fail_count_++;
            if (config_.max_starts >= 0 && start_count_ >= config_.max_starts) {
                spdlog::info("max starts ({}) reached after launch failure",
                             config_.max_starts);
                exit_code_ = 1;
                break;
            }
            continue;
        }

        bool first = (start_count_ == 1);
        auto poll_delay = first
            ? config_.effective_first_poll()
            : config_.poll_time;
        bool conn_failed = false;

        while (ssh_running() && !stop_requested()) {
            if (config_.max_lifetime > std::chrono::seconds{0}) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - daemon_start_);
                auto remaining = config_.max_lifetime - elapsed;
                if (remaining <= std::chrono::seconds{0}) break;
                poll_delay = std::min(poll_delay, remaining);
            }

            bool still_up = co_await wait_or_ssh_exit(poll_delay);
            if (!still_up || stop_requested()) break;

            if (config_.monitoring_enabled) {
                spdlog::debug("testing connection...");
                bool alive = co_await test_connection();
                if (!alive && ssh_running()) {
                    spdlog::warn("connection test failed, restarting ssh");
                    kill_ssh();
                    conn_failed = true;
                    break;
                }
                spdlog::debug("connection ok");
            }

            poll_delay = config_.poll_time;
        }

        int code = 0;
        if (conn_failed) {
            code = 255;
        } else if (ssh_) {
            error_code ec;
            if (!ssh_->running(ec))
                code = ssh_->exit_code();
        }

        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - ssh_start_);
        spdlog::info("ssh exited (code {}), uptime: {}s", code, uptime.count());

        ssh_.reset();

        if (stop_requested())
            break;
        if (consume_restart_request()) {
            spdlog::info("processing requested restart");
            continue;
        }
        if (conn_failed) {
            fast_fail_count_++;
            continue;
        }

        switch (classify_exit(code, first)) {
        case RestartAction::restart:
            continue;
        case RestartAction::exit_ok:
            exit_code_ = 0;
            break;
        case RestartAction::exit_error:
            exit_code_ = 1;
            break;
        }

        break;
    }

    kill_ssh();
    spdlog::info("autossh++ exiting");
}

// ════════════════════════════════════════════════════════════════════
//  Polling / waiting
// ════════════════════════════════════════════════════════════════════

asio::awaitable<bool>
AutoSSH::wait_or_ssh_exit(std::chrono::seconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!ssh_running() || stop_requested())
            co_return false;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto wait = std::min(remaining, std::chrono::milliseconds{2000});
        if (wait <= std::chrono::milliseconds{0}) break;

        poll_timer_.expires_after(wait);
        co_await poll_timer_.async_wait(
            asio::as_tuple(asio::use_awaitable));
    }

    co_return ssh_running() && !stop_requested();
}

// ════════════════════════════════════════════════════════════════════
//  Connection testing
// ════════════════════════════════════════════════════════════════════

asio::awaitable<bool> AutoSSH::test_connection() {
    for (int i = 0; i < MAX_CONN_TRIES; i++) {
        if (co_await test_connection_once())
            co_return true;
        if (stop_requested() || !ssh_running())
            co_return false;
        spdlog::debug("connection test attempt {}/{} failed",
                      i + 1, MAX_CONN_TRIES);
    }
    co_return false;
}

asio::awaitable<bool> AutoSSH::test_connection_once() {
    auto executor = co_await asio::this_coro::executor;

    asio::steady_timer timeout(executor);
    timeout.expires_after(config_.net_timeout);

    tcp::socket write_sock(executor);
    tcp::socket read_sock(executor);

    auto cancel = [&] {
        error_code ec;
        write_sock.close(ec);
        read_sock.close(ec);
    };

    timeout.async_wait([&](error_code ec) {
        if (!ec) {
            cancel();
            if (monitor_acceptor_) {
                error_code ec2;
                monitor_acceptor_->cancel(ec2);
            }
        }
    });

    // 1. Connect to the write (local-forward) port
    tcp::endpoint write_ep(asio::ip::make_address("127.0.0.1"),
                           config_.write_port());
    auto [conn_ec] = co_await write_sock.async_connect(
        write_ep, asio::as_tuple(asio::use_awaitable));
    if (conn_ec) { timeout.cancel(); co_return false; }

    // 2. Accept the return connection (loop method only)
    if (config_.echo_port == 0 && monitor_acceptor_) {
        auto [acc_ec] = co_await monitor_acceptor_->async_accept(
            read_sock, asio::as_tuple(asio::use_awaitable));
        if (acc_ec) { timeout.cancel(); cancel(); co_return false; }
    }

    // 3. Send test message
    auto [wec, wn] = co_await asio::async_write(
        write_sock, asio::buffer(test_message_),
        asio::as_tuple(asio::use_awaitable));
    if (wec) { timeout.cancel(); cancel(); co_return false; }

    // 4. Read response
    auto& recv = (config_.echo_port != 0) ? write_sock : read_sock;
    std::string response(test_message_.size(), '\0');
    auto [rec, rn] = co_await asio::async_read(
        recv, asio::buffer(response),
        asio::as_tuple(asio::use_awaitable));

    timeout.cancel();
    cancel();

    if (rec) co_return false;
    co_return response == test_message_;
}

// ════════════════════════════════════════════════════════════════════
//  Restart policy & backoff
// ════════════════════════════════════════════════════════════════════

AutoSSH::RestartAction AutoSSH::classify_exit(int exit_code, bool first_attempt) {
    const auto uptime = std::chrono::steady_clock::now() - ssh_start_;

    if (first_attempt && config_.gate_time > std::chrono::seconds{0} &&
        uptime <= config_.gate_time)
    {
        spdlog::error("ssh exited prematurely with status {} within gate time",
                      exit_code);
        return RestartAction::exit_error;
    }

    switch (exit_code) {
    case 255:
        fast_fail_count_++;
        return RestartAction::restart;
    case 2:
    case 1:
        if (!first_attempt || config_.gate_time == std::chrono::seconds{0}) {
            fast_fail_count_++;
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

std::chrono::seconds AutoSSH::calculate_backoff() {
    if (start_count_ == 0)
        return std::chrono::seconds{0};
    if (skip_backoff_once_)
        return std::chrono::seconds{0};

    auto uptime = std::chrono::steady_clock::now() - last_attempt_start_;
    auto min_time =
        std::max(config_.poll_time / 10, std::chrono::seconds{10});

    if (uptime >= min_time) {
        fast_fail_count_ = 0;
        return std::chrono::seconds{0};
    }

    if (fast_fail_count_ <= N_FAST_TRIES)
        return std::chrono::seconds{0};

    int n = fast_fail_count_ - N_FAST_TRIES;
    auto delay_sec =
        static_cast<int64_t>(n) * n / 3 * config_.poll_time.count() / 100;
    auto delay = std::chrono::seconds(std::max(delay_sec, static_cast<std::chrono::seconds::rep>(1)));
    return std::min(delay, config_.poll_time);
}

bool AutoSSH::check_lifetime() {
    if (config_.max_lifetime <= std::chrono::seconds{0})
        return true;

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - daemon_start_);
    if (elapsed >= config_.max_lifetime) {
        spdlog::info("max lifetime ({}s) reached", config_.max_lifetime.count());
        return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  PID file
// ════════════════════════════════════════════════════════════════════

void AutoSSH::write_pid_file() {
    if (config_.pid_file.empty()) return;

    std::ofstream f(config_.pid_file);
    if (!f) {
        spdlog::error("cannot write pid file: {}", config_.pid_file);
        return;
    }
#ifdef _WIN32
    f << _getpid() << '\n';
#else
    f << getpid() << '\n';
#endif
}

void AutoSSH::remove_pid_file() {
    if (!config_.pid_file.empty()) {
        std::error_code ec;
        std::filesystem::remove(config_.pid_file, ec);
    }
}

}  // namespace autosshpp
