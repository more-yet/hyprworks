#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::tray {
using Clock = std::chrono::steady_clock;
struct ItemKey {
    std::string service, owner, path;
    bool operator==(const ItemKey&) const = default;
};
struct Pixmap {
    std::int32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels; // Straight-alpha RGBA, tightly packed.
    bool operator==(const Pixmap&) const = default;
};
struct Icon {
    std::string name;
    std::vector<Pixmap> pixmaps;
    bool operator==(const Icon&) const = default;
};
struct Item {
    ItemKey key;
    bool available = false, item_is_menu = false;
    std::string error, id, category, title, status, icon_theme_path, menu_path;
    Icon icon, attention_icon, overlay_icon, tooltip_icon;
    std::string attention_movie, tooltip_title, tooltip_body;
    bool operator==(const Item&) const = default;
};
struct Snapshot {
    std::uint64_t revision = 0;
    std::vector<Item> items;
    bool operator==(const Snapshot&) const = default;
};
enum class Phase { stopped, connecting, synchronizing, ready, unavailable, failed };
struct Status {
    Phase phase = Phase::stopped;
    std::string error;
    bool operator==(const Status&) const = default;
};
enum class Action { activate, secondary_activate, context_menu };
enum class Outcome { acknowledged, rejected, failed, cancelled };
struct RequestResult {
    std::uint64_t id = 0;
    Outcome outcome = Outcome::cancelled;
    bool may_have_executed = false;
    std::string error;
};
struct WaitSet {
    pollfd fd{-1, 0, 0};
    std::optional<Clock::time_point> deadline;
private:
    friend class TrayConnection;
    std::uint64_t generation = 0;
};
struct Update {
    bool status_changed = false, snapshot_changed = false;
    std::optional<RequestResult> result;
};
class TrayConnection {
public:
    TrayConnection();
    ~TrayConnection();
    TrayConnection(const TrayConnection&) = delete;
    TrayConnection& operator=(const TrayConnection&) = delete;
    // Joins the current watcher by default. Ownership is explicit and never replaces another watcher.
    std::expected<void, std::string> start(bool own_watcher = false, const std::string& address = {});
    std::optional<RequestResult> stop() noexcept;
    std::expected<void, std::string> refresh();
    std::expected<std::uint64_t, std::string> action(const ItemKey& item, Action action, std::int32_t x = 0, std::int32_t y = 0);
    std::expected<std::uint64_t, std::string> scroll(const ItemKey& item, std::int32_t delta, bool horizontal = false);
    WaitSet prepare_wait();
    Update dispatch(const WaitSet& ready);
    const Status& status() const noexcept;
    const std::optional<Snapshot>& snapshot() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
