#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <poll.h>

struct wl_display;

namespace hyprworks::hyprland::shortcuts {

using Clock = std::chrono::steady_clock;

struct Shortcut {
    std::string id, description, trigger_description;
};

enum class Phase { stopped, connecting, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};

struct Event {
    std::string id;
    bool pressed = false;
    std::uint64_t seconds = 0;
    std::uint32_t nanoseconds = 0;
};

struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class ShortcutConnection;
    std::uint64_t generation = 0;
};

struct Update {
    bool status_changed = false;
    std::vector<Event> events;
};

// Drive from one host thread; no GUI is required.
class ShortcutConnection {
public:
    ShortcutConnection();
    ~ShortcutConnection();
    ShortcutConnection(const ShortcutConnection&) = delete;
    ShortcutConnection& operator=(const ShortcutConnection&) = delete;

    std::expected<void, std::string> start(const std::string& app_id, std::span<const Shortcut> shortcuts,
                                           const std::string& display = {});
    // The owner dispatches and flushes the display, and must stop this connection before destroying it.
    // Callbacks run during native dispatch and must not reenter this connection.
    // An event callback exception fails the subscription; wake must not throw.
    std::expected<void, std::string> attach(wl_display* display, const std::string& app_id, std::span<const Shortcut> shortcuts,
                                           std::function<void(Event)> on_event, std::function<void()> wake);
    void stop() noexcept;
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
