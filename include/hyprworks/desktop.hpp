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

namespace hyprworks::desktop {

using Clock = std::chrono::steady_clock;
struct Action {
    std::string id, name;
    bool operator==(const Action&) const = default;
};
struct Entry {
    std::string id, name, generic_name, comment, icon, executable, filename;
    std::vector<std::string> keywords, categories, mime_types;
    std::vector<Action> actions;
    bool visible = false, terminal = false, supports_files = false, supports_uris = false;
    bool operator==(const Entry&) const = default;
};
struct Snapshot {
    std::uint64_t revision = 0;
    std::vector<Entry> entries;
};
enum class Phase { stopped, ready, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
};
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class DesktopEntries;
    std::uint64_t generation = 0;
};
struct Update { bool status_changed = false, snapshot_changed = false; };

class DesktopEntries {
public:
    DesktopEntries();
    ~DesktopEntries();
    DesktopEntries(const DesktopEntries&) = delete;
    DesktopEntries& operator=(const DesktopEntries&) = delete;
    std::expected<void, std::string> start();
    void stop() noexcept;
    void refresh();
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;
    std::expected<void, std::string> launch(const std::string& id, std::span<const std::string> uris = {}, const std::string& action = {});
private:
    struct State;
    std::unique_ptr<State> state_;
};

}
