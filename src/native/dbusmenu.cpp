#include <hyprworks/dbusmenu.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>

namespace hyprworks::dbusmenu {
namespace {
using dbus::check;
using dbus::require;
constexpr char interface[] = "com.canonical.dbusmenu";
constexpr char properties[] = "org.freedesktop.DBus.Properties";

std::vector<std::string> read_strings(sd_bus_message* message, std::size_t& bytes) {
    std::vector<std::string> values;
    require(sd_bus_message_enter_container(message, 'a', "s"));
    while (!check(sd_bus_message_at_end(message, false))) {
        if (values.size() == 128) throw std::runtime_error("menu string array exceeds 128 entries");
        values.push_back(dbus::text(message, 's', bytes));
    }
    check(sd_bus_message_exit_container(message));
    return values;
}

void read_properties(sd_bus_message* message, Snapshot& snapshot, Item* item, std::size_t& bytes, std::size_t& binary) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
        if (++count > 128) throw std::runtime_error("menu properties exceed 128 entries");
        const auto key = dbus::text(message, 's', bytes);
        std::string* text = nullptr;
        bool* boolean = nullptr;
        const char* signature = nullptr;
        if (item) {
            if (key == "label") text = &item->label;
            else if (key == "type") text = &item->type;
            else if (key == "icon-name") text = &item->icon_name;
            else if (key == "toggle-type") text = &item->toggle_type;
            else if (key == "children-display") text = &item->children_display;
            else if (key == "disposition") text = &item->disposition;
            else if (key == "enabled") boolean = &item->enabled;
            else if (key == "visible") boolean = &item->visible;
            else if (key == "toggle-state") signature = "i";
            else if (key == "icon-data") signature = "ay";
            else if (key == "shortcut") signature = "aas";
        } else {
            if (key == "TextDirection") text = &snapshot.text_direction;
            else if (key == "Status") text = &snapshot.status;
            else if (key == "Version") signature = "u";
            else if (key == "IconThemePath") signature = "as";
        }
        if (text || boolean || signature) {
            require(sd_bus_message_enter_container(message, 'v', text ? "s" : boolean ? "b" : signature));
            if (text) *text = dbus::text(message, 's', bytes);
            else if (boolean) { int value; require(sd_bus_message_read(message, "b", &value)); *boolean = value != 0; }
            else if (key == "Version") require(sd_bus_message_read(message, "u", &snapshot.version));
            else if (key == "IconThemePath") snapshot.icon_theme_paths = read_strings(message, bytes);
            else if (key == "toggle-state") require(sd_bus_message_read(message, "i", &item->toggle_state));
            else if (key == "icon-data") {
                const void* data; std::size_t size;
                require(sd_bus_message_read_array(message, 'y', &data, &size));
                if (size > 1024 * 1024 || (binary += size) > 8 * 1024 * 1024) throw std::runtime_error("menu image budget exceeded");
                if (size) item->icon_data.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
            } else {
                require(sd_bus_message_enter_container(message, 'a', "as"));
                while (!check(sd_bus_message_at_end(message, false))) {
                    if (item->shortcuts.size() == 16) throw std::runtime_error("menu item exceeds 16 shortcuts");
                    item->shortcuts.push_back(read_strings(message, bytes));
                }
                check(sd_bus_message_exit_container(message));
            }
            check(sd_bus_message_exit_container(message));
        } else require(sd_bus_message_skip(message, "v"));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
}

void read_item(sd_bus_message* message, Snapshot& snapshot, std::int32_t parent, unsigned depth, std::size_t& bytes, std::size_t& binary) {
    if (depth > 32 || snapshot.items.size() == 2048) throw std::runtime_error("menu exceeds 2048 items or depth 32");
    require(sd_bus_message_enter_container(message, 'r', "ia{sv}av"));
    Item item;
    item.parent = parent;
    require(sd_bus_message_read(message, "i", &item.id));
    const auto id = item.id;
    if ((depth == 0 && id != 0) || (depth && id <= 0) || std::ranges::find(snapshot.items, id, &Item::id) != snapshot.items.end())
        throw std::runtime_error("invalid or duplicate menu item ID");
    read_properties(message, snapshot, &item, bytes, binary);
    snapshot.items.push_back(std::move(item));
    require(sd_bus_message_enter_container(message, 'a', "v"));
    while (check(sd_bus_message_enter_container(message, 'v', "(ia{sv}av)"))) {
        read_item(message, snapshot, id, depth + 1, bytes, binary);
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message)); check(sd_bus_message_exit_container(message));
}
}

struct MenuConnection::State {
    sd_bus* bus = nullptr;
    sd_bus_slot *owners = nullptr, *signals = nullptr, *property_signals = nullptr, *query = nullptr, *command = nullptr;
    Target target;
    Status status;
    std::optional<Snapshot> snapshot;
    Snapshot pending;
    std::optional<RequestResult> result;
    std::vector<Activation> activations;
    std::uint64_t generation = 0, last_request = 0;
    std::size_t bytes = 0, binary = 0;
    unsigned installed = 0;
    bool layout = false, dirty = false, more = false, showing = false;
    std::optional<Clock::time_point> deadline, refresh_at;
    std::exception_ptr callback_error;

    void disconnect() noexcept {
        query = sd_bus_slot_unref(query); command = sd_bus_slot_unref(command);
        owners = sd_bus_slot_unref(owners); signals = sd_bus_slot_unref(signals); property_signals = sd_bus_slot_unref(property_signals);
        bus = sd_bus_close_unref(bus); deadline.reset(); refresh_at.reset(); pending = {}; installed = 0; dirty = more = false; callback_error = {};
    }
    void invalidate() {
        dirty = true;
        if (!refresh_at) refresh_at = Clock::now() + std::chrono::milliseconds(16);
    }
    void send_query() {
        auto message = dbus::method(bus, target.owner.c_str(), target.path.c_str(), layout ? interface : properties, layout ? "GetLayout" : "GetAll");
        if (layout) {
            check(sd_bus_message_append(message.get(), "ii", 0, -1));
            check(sd_bus_message_open_container(message.get(), 'a', "s")); check(sd_bus_message_close_container(message.get()));
        } else check(sd_bus_message_append(message.get(), "s", interface));
        check(sd_bus_call_async(bus, &query, message.get(), query_reply, this, 2000000));
    }
    static int query_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.query = sd_bus_slot_unref(state.query);
        try {
            const auto* error = sd_bus_message_get_error(message);
            if (state.layout) {
                if (error) throw std::runtime_error(error->message ? error->message : error->name);
                require(sd_bus_message_read(message, "u", &state.pending.remote_revision));
                read_item(message, state.pending, -1, 0, state.bytes, state.binary);
                state.pending.revision = state.snapshot ? state.snapshot->revision : 0;
                if (!state.snapshot || state.pending != *state.snapshot) {
                    if (state.pending.revision == UINT64_MAX) throw std::runtime_error("menu revisions exhausted");
                    ++state.pending.revision; state.snapshot = std::move(state.pending);
                }
                state.pending = {}; state.deadline.reset(); state.status = {Phase::ready, {}};
            } else {
                // Some menu exporters omit the optional interface properties.
                if (!error) read_properties(message, state.pending, nullptr, state.bytes, state.binary);
                else if (!sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_INTERFACE) && !sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_METHOD))
                    throw std::runtime_error(error->message ? error->message : error->name);
                state.layout = true; state.send_query();
            }
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int installed_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            if (++state.installed == 3) { state.invalidate(); state.refresh_at = Clock::now(); }
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int owner_changed(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            const char *name, *old_owner, *new_owner;
            require(sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner));
            if (state.target.owner != name || *new_owner) return 0;
            state.status = {Phase::unavailable, "menu owner disappeared"};
            if (state.command) state.result = RequestResult{state.last_request, Outcome::failed, true, state.status.error};
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int signal(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (sd_bus_message_is_signal(message, interface, "ItemActivationRequested")) {
                Activation event;
                require(sd_bus_message_read(message, "iu", &event.id, &event.timestamp));
                if (state.activations.size() == 128) throw std::runtime_error("menu activation queue exceeds 128 entries");
                state.activations.push_back(event);
            } else state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    const Item& item(std::int32_t id) const {
        if (!bus || !snapshot || status.phase == Phase::unavailable) throw std::runtime_error("menu is unavailable");
        if (command || result) throw std::runtime_error("a menu request is already outstanding");
        if (last_request == UINT64_MAX) throw std::runtime_error("menu request identifiers exhausted");
        const auto found = std::ranges::find(snapshot->items, id, &Item::id);
        if (found == snapshot->items.end()) throw std::runtime_error("unknown menu item");
        return *found;
    }
    static int command_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.command = sd_bus_slot_unref(state.command);
        try {
            const auto* error = sd_bus_message_get_error(message);
            const bool uncertain = error && (sd_bus_error_has_name(error, SD_BUS_ERROR_NO_REPLY) || sd_bus_error_has_name(error, SD_BUS_ERROR_DISCONNECTED) || sd_bus_error_has_name(error, SD_BUS_ERROR_TIMEOUT));
            state.result = RequestResult{state.last_request, !error ? Outcome::acknowledged : uncertain ? Outcome::failed : Outcome::rejected,
                !error || uncertain, error ? (error->message ? error->message : error->name) : ""};
            if (!error && state.showing) {
                int changed; require(sd_bus_message_read(message, "b", &changed));
                if (changed) state.invalidate();
            }
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
};

MenuConnection::MenuConnection() : state_(std::make_unique<State>()) {}
MenuConnection::~MenuConnection() { stop(); }
std::expected<void, std::string> MenuConnection::start(const Target& target, const std::string& address) {
    auto& state = *state_;
    if (state.bus) return std::unexpected("menu connection is already running");
    stop(); state.snapshot.reset();
    try {
        if (!target.owner.starts_with(':') || target.owner.contains('\0') || !sd_bus_service_name_is_valid(target.owner.c_str()) ||
            target.path.contains('\0') || target.path.size() > 16384 || !sd_bus_object_path_is_valid(target.path.c_str())) throw std::runtime_error("invalid menu owner or path");
        state.target = target;
        dbus::open_session(state.bus, address);
        state.status = {Phase::connecting, {}}; state.deadline = Clock::now() + std::chrono::seconds(10);
        const auto owner_match = "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" + target.owner + "'";
        const auto match = "type='signal',sender='" + target.owner + "',path='" + target.path + "',interface='";
        check(sd_bus_add_match_async(state.bus, &state.owners, owner_match.c_str(), State::owner_changed, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.signals, (match + interface + "'").c_str(), State::signal, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.property_signals, (match + properties + "',member='PropertiesChanged'").c_str(), State::signal, State::installed_reply, &state));
        return {};
    } catch (const std::exception& error) { state.disconnect(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error); }
}
std::optional<RequestResult> MenuConnection::stop() noexcept {
    auto& state = *state_;
    auto result = std::exchange(state.result, {});
    if (state.command && !result) result = RequestResult{state.last_request, Outcome::cancelled, true, {}};
    state.disconnect(); state.status = {}; state.activations.clear(); ++state.generation;
    return result;
}
std::expected<void, std::string> MenuConnection::refresh() {
    if (!state_->bus) return std::unexpected("menu is not running");
    state_->invalidate(); ++state_->generation; return {};
}
std::expected<std::uint64_t, std::string> MenuConnection::about_to_show(std::int32_t id) {
    try {
        auto& state = *state_;
        state.item(id);
        auto message = dbus::method(state.bus, state.target.owner.c_str(), state.target.path.c_str(), interface, "AboutToShow");
        check(sd_bus_message_append(message.get(), "i", id));
        check(sd_bus_call_async(state.bus, &state.command, message.get(), State::command_reply, &state, 2000000));
        state.showing = true; ++state.generation; return ++state.last_request;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}
std::expected<std::uint64_t, std::string> MenuConnection::event(std::int32_t id, const std::string& event, std::uint32_t timestamp) {
    try {
        auto& state = *state_;
        const auto& item = state.item(id);
        if (event != "clicked" && event != "hovered" && event != "opened" && event != "closed") throw std::runtime_error("unsupported menu event");
        if (!item.visible || (event == "clicked" && (!item.enabled || item.type == "separator" || id == 0))) throw std::runtime_error("menu item is not interactive");
        auto message = dbus::method(state.bus, state.target.owner.c_str(), state.target.path.c_str(), interface, "Event");
        check(sd_bus_message_append(message.get(), "isvu", id, event.c_str(), "i", 0, timestamp));
        check(sd_bus_call_async(state.bus, &state.command, message.get(), State::command_reply, &state, 2000000));
        state.showing = false; ++state.generation; return ++state.last_request;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}
WaitSet MenuConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation;
    if (!state.bus) return wait;
    wait.deadline = state.deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    if (state.dirty && !state.query && state.installed == 3 && (!wait.deadline || *state.refresh_at < *wait.deadline)) wait.deadline = state.refresh_at;
    if (state.more || state.result || !state.activations.empty()) wait.deadline = Clock::time_point::min();
    return wait;
}
Update MenuConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1)) throw std::logic_error("dispatch requires the current menu wait set");
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.bus) return {};
    try {
        state.more = false;
        for (unsigned i = 0; i < 64; ++i) {
            const int processed = check(sd_bus_process(state.bus, nullptr));
            if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, {}));
            if (!processed || state.status.phase == Phase::unavailable) break;
            state.more = i == 63;
        }
        const auto now = Clock::now();
        if (state.status.phase == Phase::unavailable) state.disconnect();
        else {
            if (state.deadline && now >= *state.deadline) throw std::runtime_error("menu synchronization timed out");
            if (state.dirty && !state.query && state.installed == 3 && now >= *state.refresh_at) {
                state.dirty = false; state.refresh_at.reset(); state.pending = {}; state.bytes = state.binary = 0; state.layout = false;
                state.status = {Phase::synchronizing, {}}; state.deadline = now + std::chrono::seconds(10); state.send_query();
            }
        }
    } catch (const std::exception& error) {
        if (state.command && !state.result) state.result = RequestResult{state.last_request, Outcome::failed, true, error.what()};
        state.disconnect(); state.status = {Phase::failed, error.what()};
    }
    return {before != state.status, revision != (state.snapshot ? state.snapshot->revision : 0), std::exchange(state.result, {}), std::exchange(state.activations, {})};
}
const Target& MenuConnection::target() const noexcept { return state_->target; }
const Status& MenuConnection::status() const noexcept { return state_->status; }
const std::optional<Snapshot>& MenuConnection::snapshot() const noexcept { return state_->snapshot; }
}
