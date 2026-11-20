#pragma once

#include <expected>
#include <atomic>
#include <memory>
#include <span>
#include <string>

namespace hyprworks::lua {

struct InvocationState;
enum class EventLoop { headless, gui };

struct SignalSource {
    enum : unsigned { quit = 1, reload = 2 };
    int fd = -1;
    std::atomic<unsigned> pending{0};
};

class Invocation {
public:
    // The optional source must outlive the invocation. Its fd is a borrowed nonblocking
    // wake pipe; producers set pending bits before writing a byte to the pipe.
    explicit Invocation(EventLoop loop = EventLoop::headless, SignalSource* signals = nullptr);
    ~Invocation();
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    std::expected<void, std::string> load(const std::string& entry_file, std::span<const std::string> arguments = {});
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
