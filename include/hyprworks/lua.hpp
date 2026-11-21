#pragma once

#include <expected>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace hyprworks::lua {

struct InvocationState;
enum class EventLoop { headless, gui };

/// The caller owns this source and its nonblocking pipe through invocation
/// destruction. Producers publish pending bits before writing the wake byte.
struct SignalSource {
    enum : unsigned { quit = 1, reload = 2 };
    int fd = -1;
    std::atomic<unsigned> pending{0};
};

/// One configuration execution, driven on its owning thread. Cross-thread
/// quit/reload producers must finish before the invocation is destroyed.
class Invocation {
public:
    explicit Invocation(EventLoop loop = EventLoop::headless, SignalSource* signals = nullptr);
    ~Invocation();
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    std::expected<void, std::string> load(const std::string& entry_file, std::span<const std::string> arguments = {},
        std::optional<std::string> source = {});
    std::expected<void, std::string> run();
    bool running() const noexcept;
    int exit_code() const noexcept;
    void request_quit() noexcept;
    void request_reload() noexcept;
    bool reload_requested() const noexcept;
    std::optional<std::string> take_reload_source() noexcept;
    void close() noexcept;

private:
    void prepare_wait();
    void dispatch();
    std::unique_ptr<InvocationState> state_;
};

}
