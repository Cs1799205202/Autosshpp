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
    , signals_(io, SIGINT, SIGTERM)
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
    signals_.async_wait([this](error_code ec, int sig) {
        if (ec) return;
        spdlog::info("received signal {}, shutting down", sig);
        shutting_down_ = true;
        poll_timer_.cancel();
        kill_ssh();
        // Re-arm: force-exit on a second signal.
        signals_.async_wait([](error_code ec2, int) {
            if (!ec2) std::exit(1);
        });
    });
}

void AutoSSH::setup_monitor_listener() {
    auto ep = tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                            config_.read_port());
    monitor_acceptor_.emplace(io_, ep);
    error_code ec;
    monitor_acceptor_->set_option(tcp::acceptor::reuse_address(true), ec);

    spdlog::info("monitor: write port {}, read port {}",
                 config_.write_port(), config_.read_port());
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

void AutoSSH::start_ssh() {
    auto args = build_ssh_args();

    spdlog::info("starting ssh (attempt {})", start_count_ + 1);
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
                return;
            }
        }

        // Boost.Process v2: process(executor, path, args)
        ssh_.emplace(io_.get_executor(), exe, args);
        start_count_++;
        ssh_start_ = std::chrono::steady_clock::now();
    } catch (const std::exception& e) {
        spdlog::error("failed to start ssh: {}", e.what());
        ssh_.reset();
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
    auto executor = co_await asio::this_coro::executor;
    daemon_start_ = std::chrono::steady_clock::now();

    spdlog::info("autossh++ {} starting", VERSION);
    if (!config_.monitoring_enabled)
        spdlog::info("monitoring disabled (-M 0), relying on SSH ServerAlive");

    while (!shutting_down_) {
        if (config_.max_starts >= 0 && start_count_ >= config_.max_starts) {
            spdlog::info("max starts ({}) reached", config_.max_starts);
            break;
        }
        if (!check_lifetime()) break;

        auto backoff = calculate_backoff();
        if (backoff > std::chrono::seconds{0}) {
            spdlog::info("backoff: waiting {}s before restart", backoff.count());
            asio::steady_timer timer(executor);
            timer.expires_after(backoff);
            auto [ec] = co_await timer.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (shutting_down_) break;
        }

        start_ssh();
        if (!ssh_) {
            fast_fail_count_++;
            continue;
        }

        bool first = (start_count_ == 1);
        auto poll_delay = first
            ? config_.effective_first_poll()
            : config_.poll_time;
        bool conn_failed = false;

        while (ssh_running() && !shutting_down_) {
            if (config_.max_lifetime > std::chrono::seconds{0}) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - daemon_start_);
                auto remaining = config_.max_lifetime - elapsed;
                if (remaining <= std::chrono::seconds{0}) break;
                poll_delay = std::min(poll_delay, remaining);
            }

            bool still_up = co_await wait_or_ssh_exit(poll_delay);
            if (!still_up || shutting_down_) break;

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

        if (shutting_down_) break;
        if (!conn_failed && !should_restart(code, first)) {
            exit_code_ = code;
            break;
        }
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
        if (!ssh_running() || shutting_down_)
            co_return false;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto wait = std::min(remaining, std::chrono::milliseconds{2000});
        if (wait <= std::chrono::milliseconds{0}) break;

        poll_timer_.expires_after(wait);
        co_await poll_timer_.async_wait(
            asio::as_tuple(asio::use_awaitable));
    }

    co_return ssh_running() && !shutting_down_;
}

// ════════════════════════════════════════════════════════════════════
//  Connection testing
// ════════════════════════════════════════════════════════════════════

asio::awaitable<bool> AutoSSH::test_connection() {
    for (int i = 0; i < MAX_CONN_TRIES; i++) {
        if (co_await test_connection_once())
            co_return true;
        if (shutting_down_ || !ssh_running())
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

bool AutoSSH::should_restart(int exit_code, bool first_attempt) {
    if (exit_code == 0) {
        spdlog::info("ssh exited normally, not restarting");
        return false;
    }

    if (exit_code == 1 && first_attempt &&
        config_.gate_time > std::chrono::seconds{0})
    {
        auto uptime = std::chrono::steady_clock::now() - ssh_start_;
        if (uptime < config_.gate_time) {
            spdlog::error("ssh failed quickly on first attempt "
                          "(within gate time) -- likely a config error");
            return false;
        }
    }

    fast_fail_count_++;
    return true;
}

std::chrono::seconds AutoSSH::calculate_backoff() {
    if (start_count_ == 0)
        return std::chrono::seconds{0};

    auto uptime = std::chrono::steady_clock::now() - ssh_start_;
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
    auto delay = std::chrono::seconds(std::max(delay_sec, int64_t{1}));
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
