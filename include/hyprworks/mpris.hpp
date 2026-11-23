#pragma once
#include <hyprworks/action.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::mpris {

using Clock = std::chrono::steady_clock;

struct PlayerKey {
    std::string name, owner;
    bool operator==(const PlayerKey&) const = default;
};

struct Metadata {
    std::string track_id, title, album, artwork_url, url;
    std::vector<std::string> artists, album_artists, genres;
    std::optional<std::int64_t> length;
    bool operator==(const Metadata&) const = default;
};

struct Player {
    PlayerKey key;
    bool available = false;
    std::string error, identity, desktop_entry, playback_status;
    std::optional<std::string> loop_status;
    std::optional<bool> shuffle;
    std::optional<double> volume;
    double rate = 1, minimum_rate = 1, maximum_rate = 1;
    bool can_control = false, can_play = false, can_pause = false, can_seek = false;
    bool can_go_next = false, can_go_previous = false, can_raise = false, can_quit = false;
    Metadata metadata;
    std::vector<std::string> uri_schemes, mime_types;
    std::optional<std::int64_t> position;
    Clock::time_point sampled_at{};
    bool operator==(const Player&) const = default;
};

/// Extrapolates microseconds from a playing sample, bounded by known track length.
std::optional<std::int64_t> estimated_position(const Player& player, Clock::time_point now = Clock::now()) noexcept;

struct Snapshot {
    std::uint64_t revision = 0;
    std::vector<Player> players;
    bool operator==(const Snapshot&) const = default;
};

enum class Phase { stopped, connecting, synchronizing, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};

enum class Action { play, pause, play_pause, stop, next, previous, raise, quit };
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class MediaConnection;
    std::uint64_t generation = 0;
};

struct Update {
    bool status_changed = false, snapshot_changed = false;
    std::optional<ActionResult> result;
};

class MediaConnection {
public:
    MediaConnection();
    ~MediaConnection();
    MediaConnection(const MediaConnection&) = delete;
    MediaConnection& operator=(const MediaConnection&) = delete;

    std::expected<void, std::string> start(const std::string& address = {});
    std::optional<ActionResult> stop() noexcept;
    std::expected<void, std::string> refresh();
    std::expected<std::uint64_t, std::string> action(const PlayerKey& player, Action action);
    std::expected<std::uint64_t, std::string> seek(const PlayerKey& player, std::int64_t offset);
    std::expected<std::uint64_t, std::string> set_position(const PlayerKey& player, const std::string& track_id, std::int64_t position);
    std::expected<std::uint64_t, std::string> set_volume(const PlayerKey& player, double volume);
    std::expected<std::uint64_t, std::string> set_rate(const PlayerKey& player, double rate);
    std::expected<std::uint64_t, std::string> set_shuffle(const PlayerKey& player, bool shuffle);
    std::expected<std::uint64_t, std::string> set_loop_status(const PlayerKey& player, const std::string& loop_status);
    std::expected<std::uint64_t, std::string> open_uri(const PlayerKey& player, const std::string& uri);
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
