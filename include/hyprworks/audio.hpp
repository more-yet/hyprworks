#pragma once
#include <hyprworks/action.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::audio {

using Clock = std::chrono::steady_clock;
enum class Direction { output, input };
enum class DeviceState { unknown, running, idle, suspended };

struct Channel {
    std::string position;
    /// Per-channel volume, with 1.0 representing PA_VOLUME_NORM.
    double volume = 0;
    bool operator==(const Channel&) const = default;
};

struct Device {
    std::string name, description, icon_name;
    std::uint32_t index = 0;
    DeviceState state = DeviceState::unknown;
    bool mute = false, monitor = false;
    std::vector<Channel> channels;
    bool operator==(const Device&) const = default;
};

struct Snapshot {
    std::uint64_t revision = 0;
    std::string default_output, default_input;
    std::vector<Device> outputs, inputs;
    bool operator==(const Snapshot&) const = default;
};

enum class Phase { stopped, connecting, synchronizing, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};

struct WaitSet {
    std::array<pollfd, 64> fds;
    std::optional<Clock::time_point> deadline;
private:
    friend class AudioConnection;
    std::uint64_t generation = 0;
};

struct Update {
    bool status_changed = false, snapshot_changed = false;
    std::optional<ActionResult> result;
};

// Single-threaded; dispatch prepared readiness before mutating the connection.
class AudioConnection {
public:
    AudioConnection();
    ~AudioConnection();
    AudioConnection(const AudioConnection&) = delete;
    AudioConnection& operator=(const AudioConnection&) = delete;

    std::expected<void, std::string> start(const std::string& server = {});
    std::optional<ActionResult> stop() noexcept;
    std::expected<void, std::string> refresh();
    std::expected<std::uint64_t, std::string> set_volume(Direction direction, const std::string& name, std::span<const double> volumes);
    std::expected<std::uint64_t, std::string> set_mute(Direction direction, const std::string& name, bool mute);
    std::expected<std::uint64_t, std::string> set_default(Direction direction, const std::string& name);
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
