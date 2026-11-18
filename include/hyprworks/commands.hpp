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

namespace hyprworks::commands {
using Clock = std::chrono::steady_clock;
enum class Phase { stopped, connecting, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};
struct Request {
    std::uint64_t id = 0;
    std::string name, sender;
    std::vector<std::string> arguments;
};
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class CommandServer;
    std::uint64_t generation = 0;
};
struct Update {
    bool status_changed = false;
    std::vector<Request> requests;
};

class CommandServer {
public:
    CommandServer();
    ~CommandServer();
    CommandServer(const CommandServer&) = delete;
    CommandServer& operator=(const CommandServer&) = delete;
    std::expected<void, std::string> start(const std::string& name, std::span<const std::string> commands, const std::string& address = {});
    void stop() noexcept;
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    std::expected<void, std::string> respond(std::uint64_t id, const std::string& text, bool success = true);
    const Status& status() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
