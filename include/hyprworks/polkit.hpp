#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <poll.h>

namespace hyprworks::polkit {

using Clock = std::chrono::steady_clock;
enum class Phase { stopped, connecting, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
};
struct Options {
    std::string session, locale;
};
struct Identity {
    std::uint32_t uid = 0;
    std::string name;
};
struct Request {
    std::uint64_t id = 0;
    std::string action, message, icon;
    std::vector<std::pair<std::string, std::string>> details;
    std::vector<Identity> identities;
};
enum class EventKind { request, prompt, info, error, completed };
struct Event {
    EventKind kind = EventKind::request;
    std::uint64_t id = 0, prompt = 0;
    std::shared_ptr<const Request> request;
    std::string text;
    bool echo = false, authorized = false, cancelled = false;
};
struct WaitSet {
    std::array<pollfd, 9> fds;
    std::optional<Clock::time_point> deadline;
    WaitSet() { fds.fill({-1, 0, 0}); }
private:
    friend class Agent;
    std::uint64_t generation = 0;
};
struct Update {
    bool status_changed = false;
    std::vector<Event> events;
};

class Agent {
public:
    Agent();
    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    std::expected<void, std::string> start(const Options& options = {}, const std::string& address = {});
    void stop() noexcept;
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    std::expected<void, std::string> authenticate(std::uint64_t id, std::uint32_t uid);
    std::expected<void, std::string> respond(std::uint64_t id, std::uint64_t prompt, std::string_view response);
    std::expected<void, std::string> cancel(std::uint64_t id);
    bool pending(std::uint64_t id, std::uint64_t prompt = 0) const noexcept;
    const Status& status() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

}
