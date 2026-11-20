#include <hyprworks/tray.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>

namespace hyprworks::tray {
namespace {
using dbus::check;
using dbus::require;
constexpr char watcher[] = "org.kde.StatusNotifierWatcher";
constexpr char watcher_path[] = "/StatusNotifierWatcher";
constexpr char item_interface[] = "org.kde.StatusNotifierItem";
constexpr char properties[] = "org.freedesktop.DBus.Properties";

void read_pixmaps(sd_bus_message* message, std::vector<Pixmap>& pixmaps, std::size_t& binary) {
    require(sd_bus_message_enter_container(message, 'a', "(iiay)"));
    std::size_t local = 0;
    while (check(sd_bus_message_enter_container(message, 'r', "iiay"))) {
        if (pixmaps.size() == 16) throw std::runtime_error("tray icon exceeds 16 pixmaps");
        Pixmap image;
        require(sd_bus_message_read(message, "ii", &image.width, &image.height));
        const void* data; std::size_t size;
        require(sd_bus_message_read_array(message, 'y', &data, &size));
        if (image.width < 1 || image.width > 4096 || image.height < 1 || image.height > 4096 ||
            std::uint64_t(image.width) * std::uint64_t(image.height) * 4 != size || (local += size) > 8 * 1024 * 1024 || (binary += size) > 32 * 1024 * 1024)
            throw std::runtime_error("invalid tray pixmap or image budget exceeded");
        image.pixels.resize(size);
        const auto* source = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; i += 4) {
            image.pixels[i] = source[i + 1]; image.pixels[i + 1] = source[i + 2]; image.pixels[i + 2] = source[i + 3]; image.pixels[i + 3] = source[i];
        }
        pixmaps.push_back(std::move(image));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
}

void read_item(sd_bus_message* message, Item& item, std::size_t& bytes, std::size_t& binary) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
        if (++count > 128) throw std::runtime_error("tray item exceeds 128 properties");
        const auto key = dbus::text(message, 's', bytes);
        std::string* text = nullptr;
        std::vector<Pixmap>* pixmaps = nullptr;
        if (key == "Id") text = &item.id;
        else if (key == "Title") text = &item.title;
        else if (key == "Category") text = &item.category;
        else if (key == "Status") text = &item.status;
        else if (key == "IconThemePath") text = &item.icon_theme_path;
        else if (key == "Menu") text = &item.menu_path;
        else if (key == "IconName") text = &item.icon.name;
        else if (key == "AttentionIconName") text = &item.attention_icon.name;
        else if (key == "OverlayIconName") text = &item.overlay_icon.name;
        else if (key == "AttentionMovieName") text = &item.attention_movie;
        else if (key == "IconPixmap") pixmaps = &item.icon.pixmaps;
        else if (key == "AttentionIconPixmap") pixmaps = &item.attention_icon.pixmaps;
        else if (key == "OverlayIconPixmap") pixmaps = &item.overlay_icon.pixmaps;
        if (text || pixmaps || key == "ItemIsMenu" || key == "ToolTip") {
            require(sd_bus_message_enter_container(message, 'v', text ? (key == "Menu" ? "o" : "s") : pixmaps ? "a(iiay)" : key == "ItemIsMenu" ? "b" : "(sa(iiay)ss)"));
            if (text) *text = dbus::text(message, key == "Menu" ? 'o' : 's', bytes);
            else if (pixmaps) read_pixmaps(message, *pixmaps, binary);
            else if (key == "ItemIsMenu") { int value; require(sd_bus_message_read(message, "b", &value)); item.item_is_menu = value != 0; }
            else {
                require(sd_bus_message_enter_container(message, 'r', "sa(iiay)ss"));
                item.tooltip_icon.name = dbus::text(message, 's', bytes);
                read_pixmaps(message, item.tooltip_icon.pixmaps, binary);
                item.tooltip_title = dbus::text(message, 's', bytes); item.tooltip_body = dbus::text(message, 's', bytes);
                check(sd_bus_message_exit_container(message));
            }
            check(sd_bus_message_exit_container(message));
        } else require(sd_bus_message_skip(message, "v"));
        check(sd_bus_message_exit_container(message));
    }
    check(sd_bus_message_exit_container(message));
}
}

struct TrayConnection::State {
    sd_bus* bus = nullptr;
    sd_bus_slot *object = nullptr, *ownership = nullptr, *owners = nullptr, *watcher_signals = nullptr, *item_signals = nullptr, *property_signals = nullptr;
    sd_bus_slot *query = nullptr, *command = nullptr;
    Status status;
    std::optional<Snapshot> snapshot;
    Snapshot pending;
    std::optional<RequestResult> result;
    ItemKey command_item;
    std::uint64_t generation = 0, last_request = 0;
    std::size_t item = 0, bytes = 0, binary = 0;
    unsigned installed = 0;
    enum class Step { watcher_owner, register_host, list, item_owner, item_properties } step = Step::watcher_owner;
    bool own = false, acquired = false, more = false;
    std::string watcher_owner, host_name;
    std::optional<Clock::time_point> deadline, refresh_at;
    std::exception_ptr callback_error;
    std::vector<ItemKey> registrations, hosts;
    struct Registration {
        State* state;
        sd_bus_message* request;
        sd_bus_slot* slot = nullptr;
        ItemKey key;
        bool host = false, done = false;
        ~Registration() { sd_bus_slot_unref(slot); sd_bus_message_unref(request); }
    };
    std::vector<std::unique_ptr<Registration>> registering;

    void disconnect() noexcept {
        registering.clear(); registrations.clear(); hosts.clear();
        query = sd_bus_slot_unref(query); command = sd_bus_slot_unref(command);
        object = sd_bus_slot_unref(object); ownership = sd_bus_slot_unref(ownership); owners = sd_bus_slot_unref(owners);
        watcher_signals = sd_bus_slot_unref(watcher_signals); item_signals = sd_bus_slot_unref(item_signals); property_signals = sd_bus_slot_unref(property_signals);
        bus = sd_bus_close_unref(bus); pending = {}; deadline.reset(); refresh_at.reset(); installed = 0; acquired = more = false; watcher_owner.clear(); callback_error = {};
    }
    void invalidate() {
        if (!refresh_at) refresh_at = Clock::now() + std::chrono::milliseconds(16);
    }
    void publish() {
        pending.revision = snapshot ? snapshot->revision : 0;
        if (!snapshot || pending != *snapshot) {
            if (pending.revision == UINT64_MAX) throw std::runtime_error("tray revisions exhausted");
            ++pending.revision; snapshot = std::move(pending);
        }
        pending = {}; deadline.reset(); status = {Phase::ready, {}};
    }
    void send() {
        const bool owner = step == Step::watcher_owner || step == Step::item_owner;
        auto message = dbus::method(bus, owner ? "org.freedesktop.DBus" : step == Step::item_properties ? pending.items[item].key.owner.c_str() : watcher_owner.c_str(),
            owner ? "/org/freedesktop/DBus" : step == Step::item_properties ? pending.items[item].key.path.c_str() : watcher_path,
            owner ? "org.freedesktop.DBus" : step == Step::register_host ? watcher : properties,
            owner ? "GetNameOwner" : step == Step::register_host ? "RegisterStatusNotifierHost" : step == Step::list ? "Get" : "GetAll");
        if (step == Step::list) check(sd_bus_message_append(message.get(), "ss", watcher, "RegisteredStatusNotifierItems"));
        else check(sd_bus_message_append(message.get(), "s", step == Step::watcher_owner ? watcher : step == Step::item_owner ? pending.items[item].key.service.c_str() :
                                            step == Step::register_host ? host_name.c_str() : item_interface));
        check(sd_bus_call_async(bus, &query, message.get(), reply, this, 2000000));
    }
    void next_item() {
        if (++item == pending.items.size()) publish();
        else { step = Step::item_owner; send(); }
    }
    static int reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.query = sd_bus_slot_unref(state.query);
        try {
            const auto* error = sd_bus_message_get_error(message);
            if (error) {
                if (state.step == Step::watcher_owner && sd_bus_error_has_name(error, SD_BUS_ERROR_NAME_HAS_NO_OWNER)) {
                    state.watcher_owner.clear(); state.pending = {}; state.publish(); state.status = {Phase::unavailable, {}}; return 0;
                }
                if (state.step == Step::item_owner || state.step == Step::item_properties) {
                    state.pending.items[state.item].error = error->message ? error->message : error->name; state.next_item(); return 0;
                }
                throw std::runtime_error(error->message ? error->message : error->name);
            }
            if (state.step == Step::watcher_owner) {
                auto owner = dbus::text(message, 's', state.bytes);
                if (!owner.starts_with(':') || !sd_bus_service_name_is_valid(owner.c_str())) throw std::runtime_error("invalid watcher owner");
                const bool changed = owner != state.watcher_owner;
                state.watcher_owner = std::move(owner);
                state.step = changed ? Step::register_host : Step::list;
            } else if (state.step == Step::register_host) state.step = Step::list;
            else if (state.step == Step::list) {
                require(sd_bus_message_enter_container(message, 'v', "as")); require(sd_bus_message_enter_container(message, 'a', "s"));
                while (!check(sd_bus_message_at_end(message, false))) {
                    if (state.pending.items.size() == 64) throw std::runtime_error("tray exceeds 64 items");
                    const auto name = dbus::text(message, 's', state.bytes);
                    const auto slash = name.find('/');
                    Item item;
                    item.key.service = name.substr(0, slash); item.key.path = slash == std::string::npos ? "/StatusNotifierItem" : name.substr(slash);
                    if (!sd_bus_service_name_is_valid(item.key.service.c_str()) || !sd_bus_object_path_is_valid(item.key.path.c_str())) throw std::runtime_error("invalid tray registration");
                    state.pending.items.push_back(std::move(item));
                }
                check(sd_bus_message_exit_container(message)); check(sd_bus_message_exit_container(message));
                std::ranges::sort(state.pending.items, [](const Item& a, const Item& b) { return std::tie(a.key.service, a.key.path) < std::tie(b.key.service, b.key.path); });
                const auto duplicates = std::ranges::unique(state.pending.items, {}, &Item::key);
                state.pending.items.erase(duplicates.begin(), duplicates.end());
                if (state.pending.items.empty()) { state.publish(); return 0; }
                state.step = Step::item_owner;
            } else if (state.step == Step::item_owner) {
                auto& key = state.pending.items[state.item].key;
                key.owner = dbus::text(message, 's', state.bytes);
                if (!key.owner.starts_with(':') || !sd_bus_service_name_is_valid(key.owner.c_str())) throw std::runtime_error("invalid tray item owner");
                state.step = Step::item_properties;
            } else {
                auto& item = state.pending.items[state.item];
                try { read_item(message, item, state.bytes, state.binary); item.available = true; }
                catch (const std::exception& error) { item.error = error.what(); }
                state.next_item(); return 0;
            }
            state.send();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int installed_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            ++state.installed;
            state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int acquired_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            std::uint32_t code; require(sd_bus_message_read(message, "u", &code));
            if (code != 1 && code != 4) throw std::runtime_error("another tray watcher owns the name");
            state.acquired = true; state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int signal(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        const char* sender = sd_bus_message_get_sender(message);
        const char* path = sd_bus_message_get_path(message);
        const auto matches = [&](const Item& item) { return sender && path && item.key.owner == sender && item.key.path == path; };
        if ((sender && state.watcher_owner == sender && path && std::string_view(path) == watcher_path) ||
            (state.snapshot && std::ranges::any_of(state.snapshot->items, matches)) || std::ranges::any_of(state.pending.items, matches)) state.invalidate();
        return 0;
    }
    static int owner_changed(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            const char *name, *old_owner, *new_owner;
            require(sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner));
            const bool watch = std::string_view(name) == watcher;
            const auto lost = [&](const ItemKey& key) { return (key.service == name && key.owner != new_owner) || (key.owner == name && !*new_owner); };
            if (state.own) {
                for (std::size_t i = 0; i < state.registrations.size();) {
                    if (!lost(state.registrations[i])) { ++i; continue; }
                    const auto registration = state.registrations[i].service + state.registrations[i].path;
                    state.registrations.erase(state.registrations.begin() + static_cast<std::ptrdiff_t>(i));
                    check(sd_bus_emit_signal(state.bus, watcher_path, watcher, "StatusNotifierItemUnregistered", "s", registration.c_str()));
                    check(sd_bus_emit_properties_changed(state.bus, watcher_path, watcher, "RegisteredStatusNotifierItems", nullptr));
                }
                if (std::erase_if(state.hosts, lost)) check(sd_bus_emit_signal(state.bus, watcher_path, watcher, "StatusNotifierHostUnregistered", ""));
            }
            bool changed = watch;
            if (state.snapshot) {
                const auto removed = std::erase_if(state.snapshot->items, [&](const Item& item) { return watch || lost(item.key); });
                if (removed) {
                    if (state.snapshot->revision == UINT64_MAX) throw std::runtime_error("tray revisions exhausted");
                    ++state.snapshot->revision; changed = true;
                }
            }
            changed |= std::ranges::any_of(state.pending.items, [&](const Item& item) { return lost(item.key); });
            if (!changed) return 0;
            if (watch) state.watcher_owner.clear();
            if (state.command && (watch || lost(state.command_item))) {
                state.result = RequestResult{state.last_request, Outcome::failed, true, "tray owner changed"}; state.command = sd_bus_slot_unref(state.command);
            }
            state.query = sd_bus_slot_unref(state.query); state.pending = {}; state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int property(sd_bus*, const char*, const char*, const char* name, sd_bus_message* reply, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (std::strcmp(name, "ProtocolVersion") == 0) return sd_bus_message_append(reply, "i", 0);
            if (std::strcmp(name, "IsStatusNotifierHostRegistered") == 0) return sd_bus_message_append(reply, "b", true);
            check(sd_bus_message_open_container(reply, 'a', "s"));
            for (const auto& key : state.registrations) check(sd_bus_message_append(reply, "s", (key.service + key.path).c_str()));
            return sd_bus_message_close_container(reply);
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_FAILED, "%s", exception.what()); }
    }
    void register_key(const ItemKey& key, bool host) {
        auto& list = host ? hosts : registrations;
        if (std::ranges::find(list, key) != list.end()) return;
        if (list.size() == 64) throw std::runtime_error("tray watcher registration limit reached");
        list.push_back(key);
        if (host) check(sd_bus_emit_signal(bus, watcher_path, watcher, "StatusNotifierHostRegistered", ""));
        else {
            check(sd_bus_emit_signal(bus, watcher_path, watcher, "StatusNotifierItemRegistered", "s", (key.service + key.path).c_str()));
            check(sd_bus_emit_properties_changed(bus, watcher_path, watcher, "RegisteredStatusNotifierItems", nullptr));
            invalidate();
        }
    }
    static int registration_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& pending = *static_cast<Registration*>(data);
        pending.done = true;
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            std::size_t bytes = 0;
            pending.key.owner = dbus::text(message, 's', bytes);
            if (pending.key.owner != sd_bus_message_get_sender(pending.request)) throw std::runtime_error("tray registration is not owned by its sender");
            pending.state->register_key(pending.key, pending.host);
            return sd_bus_reply_method_return(pending.request, "");
        } catch (const std::exception& error) { return sd_bus_reply_method_errorf(pending.request, SD_BUS_ERROR_INVALID_ARGS, "%s", error.what()); }
    }
    static int register_request(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (!state.acquired) throw std::runtime_error("tray watcher is not ready");
            std::size_t bytes = 0;
            const auto value = dbus::text(message, 's', bytes);
            const bool host = std::strcmp(sd_bus_message_get_member(message), "RegisterStatusNotifierHost") == 0;
            ItemKey key;
            if (!host && value.starts_with('/')) {
                if (!sd_bus_object_path_is_valid(value.c_str())) throw std::runtime_error("invalid tray object path");
                key = {sd_bus_message_get_sender(message), sd_bus_message_get_sender(message), value};
                state.register_key(key, false);
                return sd_bus_reply_method_return(message, "");
            }
            const auto slash = value.find('/');
            const auto service = value.substr(0, slash);
            const auto path = slash == std::string::npos ? (host ? "" : "/StatusNotifierItem") : value.substr(slash);
            if (!sd_bus_service_name_is_valid(service.c_str()) || (!path.empty() && !sd_bus_object_path_is_valid(path.c_str()))) throw std::runtime_error("invalid tray service or path");
            if (state.registering.size() == 64) throw std::runtime_error("too many pending tray registrations");
            auto pending = std::make_unique<Registration>();
            pending->state = &state; pending->request = sd_bus_message_ref(message); pending->host = host;
            pending->key = {service, {}, path};
            auto query = dbus::method(state.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
            check(sd_bus_message_append(query.get(), "s", service.c_str()));
            check(sd_bus_call_async(state.bus, &pending->slot, query.get(), registration_reply, pending.get(), 2000000));
            state.registering.push_back(std::move(pending));
            return 1;
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s", exception.what()); }
    }
    void target(const ItemKey& key) const {
        if (!bus || !snapshot || status.phase == Phase::unavailable) throw std::runtime_error("tray is unavailable");
        if (command || result) throw std::runtime_error("a tray request is already outstanding");
        if (last_request == UINT64_MAX) throw std::runtime_error("tray request identifiers exhausted");
        const auto found = std::ranges::find(snapshot->items, key, &Item::key);
        if (found == snapshot->items.end() || !found->available) throw std::runtime_error("tray item is unavailable or its owner changed");
    }
    static int command_reply(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        state.command = sd_bus_slot_unref(state.command);
        try {
            const auto* error = sd_bus_message_get_error(message);
            const bool uncertain = error && (sd_bus_error_has_name(error, SD_BUS_ERROR_NO_REPLY) || sd_bus_error_has_name(error, SD_BUS_ERROR_DISCONNECTED) || sd_bus_error_has_name(error, SD_BUS_ERROR_TIMEOUT));
            state.result = RequestResult{state.last_request, !error ? Outcome::acknowledged : uncertain ? Outcome::failed : Outcome::rejected,
                !error || uncertain, error ? (error->message ? error->message : error->name) : ""};
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
};

TrayConnection::TrayConnection() : state_(std::make_unique<State>()) {}
TrayConnection::~TrayConnection() { stop(); }
std::expected<void, std::string> TrayConnection::start(bool own_watcher, const std::string& address) {
    auto& state = *state_;
    if (state.bus) return std::unexpected("tray connection is already running");
    stop(); state.snapshot.reset(); state.own = own_watcher;
    try {
        dbus::open(state.bus, address);
        state.status = {Phase::connecting, {}}; state.deadline = Clock::now() + std::chrono::seconds(10);
        check(sd_bus_add_match_async(state.bus, &state.owners, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", State::owner_changed, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.watcher_signals, "type='signal',interface='org.kde.StatusNotifierWatcher'", State::signal, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.item_signals, "type='signal',interface='org.kde.StatusNotifierItem'", State::signal, State::installed_reply, &state));
        check(sd_bus_add_match_async(state.bus, &state.property_signals, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", State::signal, State::installed_reply, &state));
        if (own_watcher) {
            static const sd_bus_vtable vtable[] = {
                SD_BUS_VTABLE_START(0),
                SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", State::register_request, SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", State::register_request, SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", State::property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
                SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", State::property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
                SD_BUS_PROPERTY("ProtocolVersion", "i", State::property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
                SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0), SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
                SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0), SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
                SD_BUS_VTABLE_END
            };
            check(sd_bus_add_object_vtable(state.bus, &state.object, watcher_path, watcher, vtable, &state));
            check(sd_bus_request_name_async(state.bus, &state.ownership, watcher, 0, State::acquired_reply, &state));
        } else state.acquired = true;
        return {};
    } catch (const std::exception& error) { state.disconnect(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error); }
}
std::optional<RequestResult> TrayConnection::stop() noexcept {
    auto& state = *state_;
    auto result = std::exchange(state.result, {});
    if (state.command && !result) result = RequestResult{state.last_request, Outcome::cancelled, true, {}};
    state.disconnect(); state.status = {}; ++state.generation; return result;
}
std::expected<void, std::string> TrayConnection::refresh() {
    if (!state_->bus) return std::unexpected("tray connection is not running");
    state_->invalidate(); ++state_->generation; return {};
}
std::expected<std::uint64_t, std::string> TrayConnection::action(const ItemKey& key, Action action, std::int32_t x, std::int32_t y) {
    try {
        auto& state = *state_;
        state.target(key);
        const char* member = action == Action::activate ? "Activate" : action == Action::secondary_activate ? "SecondaryActivate" : action == Action::context_menu ? "ContextMenu" : nullptr;
        if (!member) throw std::runtime_error("invalid tray action");
        auto message = dbus::method(state.bus, key.owner.c_str(), key.path.c_str(), item_interface, member);
        check(sd_bus_message_append(message.get(), "ii", x, y));
        state.command_item = key;
        check(sd_bus_call_async(state.bus, &state.command, message.get(), State::command_reply, &state, 2000000));
        ++state.generation; return ++state.last_request;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}
std::expected<std::uint64_t, std::string> TrayConnection::scroll(const ItemKey& key, std::int32_t delta, bool horizontal) {
    try {
        auto& state = *state_;
        state.target(key);
        auto message = dbus::method(state.bus, key.owner.c_str(), key.path.c_str(), item_interface, "Scroll");
        check(sd_bus_message_append(message.get(), "is", delta, horizontal ? "horizontal" : "vertical"));
        state.command_item = key;
        check(sd_bus_call_async(state.bus, &state.command, message.get(), State::command_reply, &state, 2000000));
        ++state.generation; return ++state.last_request;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}
WaitSet TrayConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation;
    if (!state.bus) return wait;
    wait.deadline = state.deadline; dbus::prepare(state.bus, wait.fd, wait.deadline);
    if (state.refresh_at && !state.query && state.installed == 4 && state.acquired && (!wait.deadline || *state.refresh_at < *wait.deadline)) wait.deadline = state.refresh_at;
    if (state.more || state.result) wait.deadline = Clock::time_point::min();
    return wait;
}
Update TrayConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1)) throw std::logic_error("dispatch requires the current tray wait set");
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.bus) return {};
    try {
        state.more = dbus::process(state.bus, state.callback_error);
        std::erase_if(state.registering, [](const auto& registration) { return registration->done; });
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline) throw std::runtime_error("tray synchronization timed out");
        if (state.refresh_at && !state.query && state.installed == 4 && state.acquired && now >= *state.refresh_at) {
            const char* unique; check(sd_bus_get_unique_name(state.bus, &unique)); state.host_name = unique;
            state.refresh_at.reset(); state.pending = {}; state.item = state.bytes = state.binary = 0;
            state.step = State::Step::watcher_owner; state.status = {Phase::synchronizing, {}};
            if (!state.deadline) state.deadline = now + std::chrono::seconds(10);
            state.send();
        }
    } catch (const std::exception& error) {
        if (state.command && !state.result) state.result = RequestResult{state.last_request, Outcome::failed, true, error.what()};
        state.disconnect(); state.status = {Phase::failed, error.what()};
    }
    return {before != state.status, revision != (state.snapshot ? state.snapshot->revision : 0), std::exchange(state.result, {})};
}
const Status& TrayConnection::status() const noexcept { return state_->status; }
const std::optional<Snapshot>& TrayConnection::snapshot() const noexcept { return state_->snapshot; }
}
