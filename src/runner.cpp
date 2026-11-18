#include <hyprworks/lua.hpp>
#include <atomic>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {
std::atomic<hyprworks::lua::Invocation*> active_invocation{nullptr};
static_assert(decltype(active_invocation)::is_always_lock_free);

void interrupt(int signal) {
    if (auto* invocation = active_invocation.load(std::memory_order_relaxed)) {
        if (signal == SIGHUP) invocation->request_reload();
        else invocation->request_quit();
    }
}

struct SignalHandlers {
    struct sigaction previous_int{}, previous_term{}, previous_hup{};

    explicit SignalHandlers(hyprworks::lua::Invocation& invocation) {
        struct sigaction action{};
        action.sa_handler = interrupt;
        sigemptyset(&action.sa_mask);
        active_invocation.store(&invocation, std::memory_order_relaxed);
        if (sigaction(SIGINT, &action, &previous_int) < 0) {
            active_invocation.store(nullptr, std::memory_order_relaxed);
            throw std::system_error(errno, std::generic_category(), "SIGINT handler");
        }
        if (sigaction(SIGTERM, &action, &previous_term) < 0) {
            const int error = errno;
            active_invocation.store(nullptr, std::memory_order_relaxed);
            sigaction(SIGINT, &previous_int, nullptr);
            throw std::system_error(error, std::generic_category(), "SIGTERM handler");
        }
        if (sigaction(SIGHUP, &action, &previous_hup) < 0) {
            const int error = errno;
            active_invocation.store(nullptr, std::memory_order_relaxed);
            sigaction(SIGINT, &previous_int, nullptr); sigaction(SIGTERM, &previous_term, nullptr);
            throw std::system_error(error, std::generic_category(), "SIGHUP handler");
        }
    }

    ~SignalHandlers() {
        active_invocation.store(nullptr, std::memory_order_relaxed);
        sigaction(SIGTERM, &previous_term, nullptr);
        sigaction(SIGHUP, &previous_hup, nullptr);
        sigaction(SIGINT, &previous_int, nullptr);
    }
    SignalHandlers(const SignalHandlers&) = delete;
    SignalHandlers& operator=(const SignalHandlers&) = delete;
};

}

int main(int argc, char** argv) {
    bool toolkit = HYPRWORKS_DEFAULT_TOOLKIT;
    const char* config_file = nullptr;
    int entry = 1;
    while (entry < argc) {
        const std::string_view option(argv[entry]);
        if (option == "--headless") toolkit = false;
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
        for (;;) {
            hyprworks::lua::Invocation invocation(toolkit ? hyprworks::lua::EventLoop::toolkit : hyprworks::lua::EventLoop::headless);
            SignalHandlers signals(invocation);
            if (auto loaded = invocation.load(filename, arguments); !loaded) {
                std::cerr << loaded.error() << '\n';
                return 1;
            }
            if (auto result = invocation.run(); !result) {
                std::cerr << result.error() << '\n';
                return 1;
            }
            if (!invocation.reload_requested()) return invocation.exit_code();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
