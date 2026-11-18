#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::upower {

using Clock = std::chrono::steady_clock;

struct Device {
    std::string path, vendor, model, icon_name;
    std::uint32_t type = 0, state = 0, warning_level = 0, battery_level = 0;
    bool present = false, power_supply = false, online = false, rechargeable = false;
    double percentage = 0, capacity = 0, energy = 0, energy_full = 0, energy_rate = 0;
    std::int64_t time_to_empty = 0, time_to_full = 0;
    bool operator==(const Device&) const = default;
};

struct Snapshot {
    std::uint64_t revision = 0;
    bool on_battery = false, lid_present = false, lid_closed = false;
    Device display;
    std::vector<Device> devices;
    bool operator==(const Snapshot&) const = default;
};

enum class Phase { stopped, connecting, synchronizing, ready, unavailable, failed };

struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};

struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class PowerConnection;
    std::uint64_t generation = 0;
};

struct Update {
    bool status_changed = false;
    bool snapshot_changed = false;
};

// Single-threaded, host-driven connection. No Lua, toolkit, or private event loop.
class PowerConnection {
public:
    PowerConnection();
    ~PowerConnection();
    PowerConnection(const PowerConnection&) = delete;
    PowerConnection& operator=(const PowerConnection&) = delete;

    // Empty address selects the system bus. An explicit address supports embedded/private buses.
    std::expected<void, std::string> start(const std::string& address = {});
    void stop() noexcept;
    void refresh();
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    const std::optional<Snapshot>& snapshot() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
