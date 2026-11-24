#include <hyprworks/lua.hpp>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
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

}

int main(int argc, char** argv) {
    bool gui = HYPRWORKS_DEFAULT_GUI;
    bool watch = true;
    const char* entry_name = "main.lua";
    int entry = 1;
    while (entry < argc) {
        const std::string_view option(argv[entry]);
        if (option == "--headless") gui = false;
        else if (option == "--no-watch") watch = false;
        else if (option == "--entry") {
            if (++entry == argc || !*argv[entry] || std::string_view(argv[entry]).contains('/') ||
                std::string_view(argv[entry]) == "." || std::string_view(argv[entry]) == "..") {
                std::cerr << "--entry requires a filename in the configuration directory\n";
                return 2;
            }
            entry_name = argv[entry];
        } else if (option == "--version" || option == "-v") {
            std::cout << "hyprworks " << HYPRWORKS_VERSION << '\n';
            return 0;
        } else if (option == "--help" || option == "-h") {
            std::cout << "usage: hyprworks [OPTIONS] [-- ARGUMENT ...]\n"
                      << "Load main.lua from $HYPRWORKS_CONFIG_DIR (required absolute directory).\n"
                      << "Reload automatically when the entry file or a required local Lua module changes.\n\n"
                      << "      --entry FILE   Select a filename in the configuration directory (main.lua)\n"
                       << "      --headless     Run the headless event loop\n"
                       << "      --no-watch     Keep this invocation across configuration file changes\n"
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
        const char* directory = std::getenv("HYPRWORKS_CONFIG_DIR");
        if (!directory || !std::filesystem::path(directory).is_absolute())
            throw std::runtime_error("set HYPRWORKS_CONFIG_DIR to an absolute directory");
        const auto filename = (std::filesystem::path(directory) / entry_name).lexically_normal().string();
        std::vector<std::string> arguments(argv + entry, argv + argc);
        start_signals();
        for (;;) {
            hyprworks::lua::Invocation invocation(gui ? hyprworks::lua::EventLoop::gui : hyprworks::lua::EventLoop::headless, &signals);
            if (auto loaded = invocation.load(filename, arguments, watch); !loaded) {
                std::cerr << loaded.error() << '\n';
                return 1;
            }
            if (auto result = invocation.run(); !result) {
                std::cerr << result.error() << '\n';
                return 1;
            }
            if (!invocation.reload_requested() || (signals.pending.load(std::memory_order_relaxed) & hyprworks::lua::SignalSource::quit))
                return invocation.exit_code();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
