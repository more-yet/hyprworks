#pragma once

#include <expected>
#include <atomic>
#include <memory>
#include <span>
#include <string>

namespace hyprworks::lua {

struct InvocationState;
enum class EventLoop { headless, gui };

// Outlives Invocation; publish pending bits before waking the nonblocking pipe.
struct SignalSource {
    enum : unsigned { quit = 1, reload = 2 };
    int fd = -1;
    std::atomic<unsigned> pending{0};
};

// Single-threaded except quit/reload requests; stop producers before destruction.
class Invocation {
public:
    explicit Invocation(EventLoop loop = EventLoop::headless, SignalSource* signals = nullptr);
    ~Invocation();
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    std::expected<void, std::string> load(const std::string& entry_file, std::span<const std::string> arguments = {}, bool watch = true);
    std::expected<void, std::string> run();
    bool running() const noexcept;
    int exit_code() const noexcept;
    void request_quit() noexcept;
    void request_reload() noexcept;
    bool reload_requested() const noexcept;
    void close() noexcept;

private:
    void prepare_wait();
    void dispatch();
    std::unique_ptr<InvocationState> state_;
};

}
