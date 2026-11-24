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
class FileDescriptor {
public:
    FileDescriptor() = default;
    static std::expected<FileDescriptor, std::string> duplicate(int fd);
    int get() const noexcept;
private:
    struct Owned;
    std::shared_ptr<const Owned> fd_;
};
struct Value {
    std::string signature;
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::vector<Value>, FileDescriptor> data;
};
using Body = std::vector<Value>;
struct Method { std::string name, input, output; };
struct Signal { std::string name, signature; };
struct Property { std::string name; Value value; bool constant = false; };
struct Call {
    std::string destination, path, interface, member;
    Body arguments;
    std::chrono::milliseconds timeout{};
    bool interactive = false;
};
enum class EventKind { reply, signal, method, expired, name_acquired, name_lost, name_failed, peer_gone };
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
    std::expected<void, std::string> request_name(const std::string& name);
    std::expected<void, std::string> release_name(const std::string& name);
    std::vector<std::string> owned_names() const;
    std::expected<void, std::string> export_interface(const std::string& path, const std::string& interface,
        const std::vector<Method>& methods, const std::vector<Signal>& signals,
        const std::vector<Property>& properties, std::chrono::milliseconds timeout);
    std::expected<void, std::string> unexport(const std::string& path, const std::string& interface);
    std::expected<void, std::string> emit(const std::string& path, const std::string& interface,
        const std::string& member, const Body& arguments, const std::string& destination = {});
    std::expected<void, std::string> set_property(const std::string& path, const std::string& interface,
        const std::string& name, const Value& value);
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
