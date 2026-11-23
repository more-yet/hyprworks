#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <poll.h>

namespace hyprworks::dbus {

using Clock = std::chrono::steady_clock;
struct Value {
    std::string signature;
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::vector<Value>> data;
};
using Body = std::vector<Value>;
struct Method { std::string name, input, output; };
struct Call {
    std::string destination, path, interface, member;
    Body arguments;
    std::chrono::milliseconds timeout{};
    bool interactive = false;
};
enum class EventKind { reply, signal, method, expired };
struct Event {
    EventKind kind = EventKind::reply;
    std::uint64_t id = 0;
    std::string sender{}, path{}, interface{}, member{}, error_name{}, error{};
    Body arguments{};
};
enum class Phase { stopped, connecting, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class Connection;
    std::uint64_t generation = 0;
};
struct Update { bool status_changed = false; std::vector<Event> events; };

class Connection {
public:
    Connection();
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    std::expected<void, std::string> start(bool system, const std::string& address = {});
    void stop() noexcept;
    std::expected<std::uint64_t, std::string> call(const Call& call);
    std::expected<void, std::string> cancel(std::uint64_t id);
    std::expected<std::uint64_t, std::string> subscribe(const std::string& match);
    std::expected<void, std::string> unsubscribe(std::uint64_t id);
    std::expected<void, std::string> export_methods(const std::string& path, const std::string& interface,
        const std::vector<Method>& methods, std::chrono::milliseconds timeout);
    std::expected<void, std::string> reply(std::uint64_t id, const Body& arguments);
    std::expected<void, std::string> error(std::uint64_t id, const std::string& name, const std::string& message);
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

}
