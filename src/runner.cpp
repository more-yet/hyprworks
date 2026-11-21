#include <hyprworks/lua.hpp>
#include <hyprworks/host.hpp>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
// Process-lifetime storage keeps signal delivery valid across invocation teardown.
hyprworks::lua::SignalSource signals;
int signal_write_fd = -1;

void receive_signal(int signal) {
    const int saved = errno;
    signals.pending.fetch_or(signal == SIGHUP ? hyprworks::lua::SignalSource::reload :
        hyprworks::lua::SignalSource::quit, std::memory_order_relaxed);
    const char wake = 0;
    while (write(signal_write_fd, &wake, 1) < 0 && errno == EINTR) {}
    errno = saved;
}

void start_signals() {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    int pipe[2];
    if (pipe2(pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "create signal pipe");
    signals.fd = pipe[0]; signal_write_fd = pipe[1];
    struct sigaction action{};
    action.sa_handler = receive_signal;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&action.sa_mask, signal);
    for (const int signal : {SIGINT, SIGTERM, SIGHUP})
        if (sigaction(signal, &action, nullptr) < 0)
            throw std::system_error(errno, std::generic_category(), "install runtime signal handler");
    if (sigprocmask(SIG_UNBLOCK, &action.sa_mask, nullptr) < 0)
        throw std::system_error(errno, std::generic_category(), "unblock runtime signals");
}

bool wait_for_correction(hyprworks::host::FileWatch& watch, const std::string& filename) {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> retry_at;
    std::cerr << (watch.running() ? "Hyprworks: waiting for an entry-file edit or SIGHUP\n" :
        "Hyprworks: waiting for SIGHUP\n");
    for (;;) {
        for (unsigned i = 0; i < 16; ++i) {
            char bytes[256];
            const auto count = read(signals.fd, bytes, sizeof(bytes));
            if (count > 0) continue;
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            throw std::system_error(count < 0 ? errno : EPIPE, std::generic_category(), "read runtime signals");
        }
        const auto pending = signals.pending.exchange(0, std::memory_order_acq_rel);
        if (pending & hyprworks::lua::SignalSource::quit) return false;
        if (pending & hyprworks::lua::SignalSource::reload) return true;
        const auto now = Clock::now();
        if (retry_at && now >= *retry_at) return true;
        pollfd fds[]{{signals.fd, POLLIN, 0}, watch.prepare_wait()};
        timespec timeout{};
        if (retry_at) {
            const auto ns = std::chrono::ceil<std::chrono::nanoseconds>(*retry_at - now);
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ns);
            timeout = {seconds.count(), (ns - seconds).count()};
        }
        if (ppoll(fds, 2, retry_at ? &timeout : nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "wait for configuration correction");
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("runtime signal descriptor failed");
        if (fds[1].revents) {
            const auto update = watch.dispatch();
            if (update.changed) retry_at = Clock::now() + std::chrono::milliseconds(50);
            if (update.invalidated) {
                if (auto resumed = watch.start(filename); !resumed)
                    std::cerr << "Hyprworks: configuration watch: " << resumed.error() << '\n';
            }
        }
    }
}

}

int main(int argc, char** argv) {
    bool gui = HYPRWORKS_DEFAULT_GUI;
    const char* config_file = nullptr;
    int entry = 1;
    while (entry < argc) {
        const std::string_view option(argv[entry]);
        if (option == "--headless") gui = false;
        else if (option == "--config" || option == "-c") {
            if (++entry == argc || !*argv[entry]) {
                std::cerr << option << " requires a Lua configuration file\n";
                return 2;
            }
            config_file = argv[entry];
        } else if (option == "--version" || option == "-v") {
            std::cout << "hyprworks " << HYPRWORKS_VERSION << '\n';
            return 0;
        } else if (option == "--help" || option == "-h") {
            std::cout << "usage: hyprworks [OPTIONS] [-- ARGUMENT ...]\n"
                      << "Load $XDG_CONFIG_HOME/hyprworks/main.lua\n"
                      << "(default: $HOME/.config/hyprworks/main.lua).\n"
                      << "Reload automatically when the entry file changes.\n\n"
                      << "  -c, --config FILE  Use another Lua configuration file\n"
                      << "      --headless     Run without the GUI event loop\n"
                      << "  -v, --version      Print the version\n"
                      << "  -h, --help         Show this help\n";
            return 0;
        } else if (option == "--") {
            ++entry;
            break;
        } else {
            std::cerr << "unknown argument: " << option << " (see --help)\n";
            return 2;
        }
        ++entry;
    }
    try {
        std::filesystem::path entry_file;
        if (config_file)
            entry_file = config_file;
        else {
            const char* config = std::getenv("XDG_CONFIG_HOME");
            if (config && std::filesystem::path(config).is_absolute())
                entry_file = std::filesystem::path(config) / "hyprworks/main.lua";
            else {
                const char* home = std::getenv("HOME");
                if (!home || !std::filesystem::path(home).is_absolute())
                    throw std::runtime_error("set an absolute XDG_CONFIG_HOME or HOME, or use --config FILE");
                entry_file = std::filesystem::path(home) / ".config/hyprworks/main.lua";
            }
        }
        const auto filename = std::filesystem::absolute(entry_file).lexically_normal().string();
        std::vector<std::string> arguments(argv + entry, argv + argc);
        start_signals();
        hyprworks::host::FileWatch recovery_watch;
        std::optional<std::string> source;
        for (;;) {
            if (auto watched = recovery_watch.start(filename); !watched)
                std::cerr << "Hyprworks: configuration watch: " << watched.error() << '\n';
            hyprworks::lua::Invocation invocation(gui ? hyprworks::lua::EventLoop::gui : hyprworks::lua::EventLoop::headless, &signals);
            if (auto loaded = invocation.load(filename, arguments, std::exchange(source, {})); !loaded) {
                std::cerr << loaded.error() << '\n';
                if (!wait_for_correction(recovery_watch, filename)) return 0;
                continue;
            }
            recovery_watch.stop();
            if (auto result = invocation.run(); !result) {
                std::cerr << result.error() << '\n';
                return 1;
            }
            if (!invocation.reload_requested() || (signals.pending.load(std::memory_order_relaxed) & hyprworks::lua::SignalSource::quit))
                return invocation.exit_code();
            source = invocation.take_reload_source();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
