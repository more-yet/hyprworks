#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::lua {

struct InvocationState;
enum class EventLoop { headless, toolkit };

struct WaitSet {
    std::vector<pollfd> fds;
    std::optional<std::chrono::steady_clock::time_point> deadline;

private:
    friend class Invocation;
    std::uint64_t generation = 0;
};

class Invocation {
public:
    explicit Invocation(EventLoop loop = EventLoop::headless);
    ~Invocation();
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    std::expected<void, std::string> load(const std::string& entry_file, std::span<const std::string> arguments = {});
    std::expected<void, std::string> run();
    WaitSet prepare_wait();
    std::expected<void, std::string> dispatch(const WaitSet& ready);
    bool running() const noexcept;
    int exit_code() const noexcept;
    void request_quit() noexcept;
    void request_reload() noexcept;
    bool reload_requested() const noexcept;
    void close() noexcept;

private:
    std::unique_ptr<InvocationState> state_;
};

}
