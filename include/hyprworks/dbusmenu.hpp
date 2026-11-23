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

namespace hyprworks::dbusmenu {

using Clock = std::chrono::steady_clock;

struct Target {
    std::string owner, path;
    bool operator==(const Target&) const = default;
};
struct Item {
    std::int32_t id = 0, parent = -1;
    std::string label, type = "standard", icon_name, toggle_type, children_display, disposition = "normal";
    bool enabled = true, visible = true;
    std::int32_t toggle_state = -1;
    std::vector<std::uint8_t> icon_data;
    std::vector<std::vector<std::string>> shortcuts;
    bool operator==(const Item&) const = default;
};
struct Snapshot {
    std::uint64_t revision = 0;
    std::uint32_t remote_revision = 0, version = 0;
    std::string text_direction = "ltr", status = "normal";
    std::vector<std::string> icon_theme_paths;
    /// Preorder records use parent item IDs; the root ID is zero.
    std::vector<Item> items;
    bool operator==(const Snapshot&) const = default;
};
enum class Phase { stopped, connecting, synchronizing, ready, unavailable, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};
struct Activation { std::int32_t id = 0; std::uint32_t timestamp = 0; };
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class MenuConnection;
    std::uint64_t generation = 0;
};
struct Update {
    bool status_changed = false, snapshot_changed = false;
    std::optional<ActionResult> result;
    std::vector<Activation> activations;
};

class MenuConnection {
public:
    MenuConnection();
    ~MenuConnection();
    MenuConnection(const MenuConnection&) = delete;
    MenuConnection& operator=(const MenuConnection&) = delete;
    std::expected<void, std::string> start(const Target& target, const std::string& address = {});
    std::optional<ActionResult> stop() noexcept;
    std::expected<void, std::string> refresh();
    std::expected<std::uint64_t, std::string> about_to_show(std::int32_t id);
    std::expected<std::uint64_t, std::string> event(std::int32_t id, const std::string& event, std::uint32_t timestamp);
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Target& target() const noexcept;
    const Status& status() const noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

}
