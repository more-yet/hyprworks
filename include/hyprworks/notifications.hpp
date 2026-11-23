#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <poll.h>

namespace hyprworks::notifications {

using Clock = std::chrono::steady_clock;

struct Image {
    std::int32_t width = 0, height = 0, row_stride = 0, channels = 0;
    bool alpha = false;
    std::vector<std::uint8_t> pixels;
};
using Hint = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::vector<std::string>, std::vector<std::uint8_t>, Image>;
struct Action { std::string key, label; };
struct Notification {
    std::uint32_t id = 0;
    std::string sender, app_name, app_icon, summary, body;
    std::vector<Action> actions;
    std::map<std::string, Hint> hints;
    std::optional<Clock::time_point> expires_at;
};

struct Options {
    std::string name, vendor, version;
    std::vector<std::string> capabilities;
    std::optional<std::chrono::milliseconds> default_timeout;
};

enum class Phase { stopped, connecting, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};
enum class CloseReason : std::uint32_t { expired = 1, dismissed = 2, requested = 3, undefined = 4 };
enum class EventKind { received, closed, action };
struct Event {
    EventKind kind = EventKind::received;
    std::uint32_t id = 0;
    CloseReason reason = CloseReason::undefined;
    std::string action;
    bool replaced = false;
};
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class NotificationServer;
    std::uint64_t generation = 0;
};
struct Update {
    bool status_changed = false;
    std::vector<Event> events;
};

class NotificationServer {
public:
    NotificationServer();
    ~NotificationServer();
    NotificationServer(const NotificationServer&) = delete;
    NotificationServer& operator=(const NotificationServer&) = delete;
    std::expected<void, std::string> start(const Options& options, const std::string& address = {});
    void stop() noexcept;
    std::expected<void, std::string> close(std::uint32_t id, CloseReason reason);
    std::expected<void, std::string> invoke(std::uint32_t id, const std::string& action, const std::string& activation_token = {});
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    const std::vector<Notification>& notifications() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

}
