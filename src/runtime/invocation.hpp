#pragma once

#include <hyprworks/lua.hpp>
#include <hyprworks/hyprland/workspaces.hpp>
#include <hyprworks/host.hpp>
#ifdef HYPRWORKS_HAS_DBUS
#include <hyprworks/dbus.hpp>
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
#include <hyprworks/desktop.hpp>
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
#include <hyprworks/hyprland/shortcuts.hpp>
#endif
#ifdef HYPRWORKS_HAS_TRAY
#include <hyprworks/tray.hpp>
#include <hyprworks/dbusmenu.hpp>
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
#include <hyprworks/notifications.hpp>
#endif
#ifdef HYPRWORKS_HAS_MPRIS
#include <hyprworks/mpris.hpp>
#endif
#ifdef HYPRWORKS_HAS_AUDIO
#include <hyprworks/audio.hpp>
#endif
#ifdef HYPRWORKS_HAS_UPOWER
#include <hyprworks/upower.hpp>
#endif
#ifdef HYPRWORKS_HAS_POLKIT
#include <hyprworks/polkit.hpp>
#endif

#include "../lua/binding.hpp"
#include <atomic>
#include <algorithm>
#include <array>

namespace hyprworks::lua {

namespace native = hyprworks::hyprland;
struct Gui;
struct WaitSet {
    std::vector<pollfd> fds;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    void due(std::optional<native::TimePoint> at) {
        if (at && (!deadline || at < deadline)) deadline = at;
    }
    std::size_t append(pollfd fd) {
        const auto index = fds.size();
        fds.push_back(fd);
        return index;
    }
};

template<class T>
auto& vacant_slot(std::vector<std::shared_ptr<T>>& active) {
    const auto slot = std::ranges::find(active, nullptr);
    if (slot != active.end()) return *slot;
    if (active.size() == T::limit) throw std::runtime_error("active handle limit reached (" + std::to_string(T::limit) + ")");
    // The slot stays at this address across Lua allocation and reentrant opens.
    if (active.empty()) active.reserve(T::limit);
    return active.emplace_back();
}

template<class T>
int release_userdata(lua_State* lua) {
    // A finalizer can resurrect the userdata after its owner is released.
    lua_pushnil(lua); lua_setmetatable(lua, 1);
    std::destroy_at(static_cast<std::shared_ptr<T>*>(lua_touserdata(lua, 1)));
    return 0;
}

template<class T>
int open_active(lua_State* lua, std::shared_ptr<T>& slot, const std::shared_ptr<T>& binding, const char* metatable,
                int* initial_callback = nullptr) {
    if (!lua_checkstack(lua, 4)) throw std::runtime_error("cannot grow Lua stack");
    slot = binding;
    if (initial_callback) lua_pushvalue(lua, 2);
    const auto result = protected_call(lua, [&](lua_State* lua) {
        auto* handle = new (lua_newuserdatauv(lua, sizeof(std::shared_ptr<T>), 0)) std::shared_ptr<T>();
        luaL_setmetatable(lua, metatable);
        if (initial_callback) { lua_pushvalue(lua, 2); *initial_callback = luaL_ref(lua, LUA_REGISTRYINDEX); }
        lua_pushvalue(lua, -1); binding->handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        *handle = binding;
        return 1;
    }, 1, 0, initial_callback ? 1 : 0);
    if (result < 0) {
        unref(lua, binding->handle);
        if (initial_callback) unref(lua, *initial_callback);
        slot.reset();
    }
    return result;
}

template<class T>
void untrack(lua_State* lua, std::vector<std::shared_ptr<T>>& active, T& binding) {
    unref(lua, binding.handle);
    for (auto& slot : active) if (slot.get() == &binding) { slot.reset(); break; }
    binding.closed = true;
}

template<class T>
int resume_handle(lua_State* lua, std::vector<std::shared_ptr<T>>& active, T& binding) {
    if (!binding.closed) return 0;
    auto& slot = vacant_slot(active);
    const auto& handle = *static_cast<std::shared_ptr<T>*>(lua_touserdata(lua, 1));
    lua_pushvalue(lua, 1);
    const auto generation = binding.generation;
    int ref;
    if (protected_call(lua, [&](lua_State* lua) { ref = luaL_ref(lua, LUA_REGISTRYINDEX); return 0; }, 0, 0, 1) < 0) return -1;
    if (slot || binding.generation != generation) {
        luaL_unref(lua, LUA_REGISTRYINDEX, ref);
        throw std::runtime_error("handle changed during restart");
    }
    slot = handle; binding.handle = ref; binding.closed = false;
    return 0;
}

struct TimerBinding {
    static constexpr std::size_t limit = 128;
    host::Timer timer;
    int handle = LUA_NOREF, callback = LUA_NOREF;
    std::uint64_t pending = 0, generation = 0;
    bool closed = false;
};

struct ProcessBinding {
    static constexpr std::size_t limit = 64;
    host::Process process;
    host::ProcessWait wait;
    std::size_t offset = 0;
    int handle = LUA_NOREF, on_exit = LUA_NOREF, on_output = LUA_NOREF, on_drain = LUA_NOREF;
    std::array<std::size_t, 2> delivered{};
    bool drained = false;
    bool output_pending() const {
        return on_output >= 0 && (delivered[0] < process.output(false).size() || delivered[1] < process.output(true).size());
    }
    std::uint64_t generation = 0;
    bool pending = false, closed = false;
};

struct WatchBinding {
    static constexpr std::size_t limit = 64;
    host::FileWatch watch;
    std::size_t offset = 0;
    int handle = LUA_NOREF, callback = LUA_NOREF;
    host::FileUpdate pending;
    std::uint64_t generation = 0;
    bool closed = false;
};

#ifdef HYPRWORKS_HAS_DESKTOP
struct DesktopBinding {
    desktop::DesktopEntries entries;
    desktop::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending; }
    void retain(desktop::Update update) { status_pending |= update.status_changed; change_pending |= update.snapshot_changed; }
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, status_pending = false, change_pending = false;
};
#endif

#ifdef HYPRWORKS_HAS_SHORTCUTS
namespace shortcuts = hyprworks::hyprland::shortcuts;
struct ShortcutBinding {
    shortcuts::ShortcutConnection connection;
    shortcuts::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || !events.empty(); }
    void retain(shortcuts::Update update) { status_pending |= update.status_changed; events = std::move(update.events); }
    std::string app_id, display;
    std::vector<shortcuts::Shortcut> shortcuts;
    std::vector<shortcuts::Event> events;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_event = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, status_pending = false;
};
#endif

struct WorkspaceBinding {
    static constexpr std::size_t limit = 32;
    native::SessionTarget target;
    native::WorkspaceConnection connection;
    native::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending || result; }
    void retain(native::DispatchResult update) {
        status_pending |= update.status_changed; change_pending |= update.snapshot_changed;
        if (update.switch_result) result = std::move(update.switch_result);
    }
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF, on_result = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false;
    bool status_pending = false, change_pending = false;
    std::optional<native::SwitchResult> result;
};

#ifdef HYPRWORKS_HAS_TRAY
struct TrayBinding {
    tray::TrayConnection connection;
    tray::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending || result; }
    void retain(tray::Update update) {
        status_pending |= update.status_changed; change_pending |= update.snapshot_changed;
        if (update.result) result = std::move(update.result);
    }
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, own_watcher = false, status_pending = false, change_pending = false;
    std::uint64_t generation = 0;
    std::optional<ActionResult> result;
};
struct MenuBinding {
    static constexpr std::size_t limit = 32;
    dbusmenu::MenuConnection connection;
    dbusmenu::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending || result || !activations.empty(); }
    void retain(dbusmenu::Update update) {
        status_pending |= update.status_changed; change_pending |= update.snapshot_changed;
        if (update.result) result = std::move(update.result);
        activations = std::move(update.activations);
    }
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF, on_result = LUA_NOREF, on_activation = LUA_NOREF;
    bool closed = false, status_pending = false, change_pending = false;
    std::uint64_t generation = 0;
    std::optional<ActionResult> result;
    std::vector<dbusmenu::Activation> activations;
};
#endif

#ifdef HYPRWORKS_HAS_NOTIFICATIONS
struct NotificationBinding {
    notifications::NotificationServer server;
    notifications::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || !events.empty(); }
    void retain(notifications::Update update) { status_pending |= update.status_changed; events = std::move(update.events); }
    notifications::Options options;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_event = LUA_NOREF;
    bool closed = false, status_pending = false;
    std::uint64_t generation = 0;
    std::vector<notifications::Event> events;
};
#endif

#ifdef HYPRWORKS_HAS_MPRIS
struct MediaBinding {
    mpris::MediaConnection connection;
    mpris::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending || result; }
    void retain(mpris::Update update) {
        status_pending |= update.status_changed; change_pending |= update.snapshot_changed;
        if (update.result) result = std::move(update.result);
    }
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, change_pending = false, status_pending = false;
    std::uint64_t generation = 0;
    std::optional<ActionResult> result;
};
#endif

#ifdef HYPRWORKS_HAS_AUDIO
struct AudioBinding {
    audio::AudioConnection connection;
    audio::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending || result; }
    void retain(audio::Update update) {
        status_pending |= update.status_changed; change_pending |= update.snapshot_changed;
        if (update.result) result = std::move(update.result);
    }
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, change_pending = false, status_pending = false;
    std::uint64_t generation = 0;
    std::optional<ActionResult> result;
};
#endif

#ifdef HYPRWORKS_HAS_UPOWER
struct PowerBinding {
    upower::PowerConnection connection;
    upower::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || change_pending; }
    void retain(upower::Update update) { status_pending |= update.status_changed; change_pending |= update.snapshot_changed; }
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, change_pending = false, status_pending = false;
};
#endif

#ifdef HYPRWORKS_HAS_POLKIT
struct PolkitBinding {
    polkit::Agent agent;
    polkit::Options options;
    polkit::WaitSet wait;
    std::size_t offset = 0;
    bool pending() const { return status_pending || !events.empty(); }
    void retain(polkit::Update update) { status_pending |= update.status_changed; events = std::move(update.events); }
    std::vector<polkit::Event> events;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_event = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, status_pending = false;
};
#endif

/// Stable binding storage spans native dispatch, reentrant Lua finalizers and
/// lua_close. Native resources stop first; binding storage is reclaimed last.
struct InvocationState {
    InvocationState();
    ~InvocationState();
    lua_State* lua = nullptr;
    EventLoop loop = EventLoop::headless;
    std::atomic<unsigned> requests{0};
    std::string configuration_directory;
    int wake_fd = -1;
    int clock_fd = -1, on_clock_change = LUA_NOREF;
    std::size_t clock_offset = 0, timezone_offset = 0, reload_offset = 0;
    bool clock_pending = false;
    host::FileWatch timezone_watch;
    SignalSource* signals = nullptr;
    bool loaded = false;
    bool closing = false;
    int exit_code = 0;
    WaitSet wait;
    std::vector<std::shared_ptr<WorkspaceBinding>> workspaces;
    std::vector<std::shared_ptr<TimerBinding>> timers;
    std::vector<std::shared_ptr<ProcessBinding>> processes;
    std::vector<std::shared_ptr<WatchBinding>> watches;
#ifdef HYPRWORKS_HAS_DBUS
    struct BusBinding {
        static constexpr std::size_t limit = 8;
        dbus::Connection connection;
        dbus::WaitSet wait;
        std::size_t offset = 0;
        bool pending() const { return status_pending || !events.empty(); }
        void retain(dbus::Update update) { status_pending |= update.status_changed; events = std::move(update.events); }
        std::vector<dbus::Event> events;
        int handle = LUA_NOREF, on_event = LUA_NOREF, on_status = LUA_NOREF;
        std::uint64_t generation = 0;
        bool closed = false, status_pending = true;
    };
    std::vector<std::shared_ptr<BusBinding>> buses;
#endif
    host::FileWatch reload_watch;
    std::optional<host::Clock::time_point> reload_at;
#ifdef HYPRWORKS_HAS_DESKTOP
    std::optional<DesktopBinding> desktop;
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    std::optional<ShortcutBinding> shortcuts;
#endif
#ifdef HYPRWORKS_HAS_TRAY
    std::optional<TrayBinding> tray;
    std::vector<std::shared_ptr<MenuBinding>> menus;
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    std::optional<NotificationBinding> notifications;
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    std::optional<MediaBinding> media;
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    std::optional<AudioBinding> audio;
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    std::optional<PowerBinding> power;
#endif
#ifdef HYPRWORKS_HAS_GUI
    std::unique_ptr<Gui> gui;
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    std::optional<PolkitBinding> polkit;
#endif
};

inline InvocationState& invocation(lua_State* lua) {
    return **static_cast<InvocationState**>(lua_getextraspace(lua));
}

template<class Binding, class Arguments, class Admit = std::nullptr_t>
int dispatch_callback(lua_State* lua, Binding& binding, const int& callback,
    std::uint64_t generation, Arguments arguments, Admit admit = nullptr) {
    auto& state = invocation(lua);
    if (callback < 0 || binding.closed || binding.generation != generation ||
        state.requests.load(std::memory_order_relaxed)) return 0;
    const int reference = callback;
    lua_pushcfunction(lua, traceback);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
    const int result = protected_call(lua, [&](lua_State* lua) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
        const int count = arguments(lua);
        // Argument allocation can run finalizers that replace or close this binding.
        if (binding.closed || binding.generation != generation || state.requests.load(std::memory_order_relaxed) ||
            !callback_matches(lua, callback, reference, count + 1)) return 0;
        if constexpr (!std::is_same_v<Admit, std::nullptr_t>)
            if (!admit()) return 0;
        lua_call(lua, count + 1, 0);
        return 0;
    }, 0, 1, 1);
    if (result >= 0) lua_settop(lua, 0);
    return result;
}

template<class Binding, class... Options>
std::expected<void, std::string> restart_observer(Binding& binding, const Options&... options) {
    if (binding.result) return std::unexpected("a result awaits delivery");
    const auto before = binding.connection.status();
    const bool published = bool(binding.connection.snapshot());
    const auto result = binding.connection.start(options...);
    binding.closed = false;
    if (result || before != binding.connection.status()) {
        ++binding.generation;
        binding.status_pending = true;
    }
    binding.change_pending |= published && !binding.connection.snapshot();
    return result;
}

template<class Binding, class Push = std::nullptr_t>
int deliver_observer(lua_State* lua, Binding& binding, Push push_result = nullptr) {
    const auto generation = binding.generation;
    const bool status = std::exchange(binding.status_pending, false), change = std::exchange(binding.change_pending, false);
    const auto no_args = [](lua_State*) { return 0; };
    if (status && dispatch_callback(lua, binding, binding.on_status, generation, no_args) < 0) return -1;
    if (change && dispatch_callback(lua, binding, binding.on_change, generation, no_args) < 0) return -1;
    if constexpr (requires { binding.result; }) {
        if (binding.result && !binding.closed && binding.generation == generation && !invocation(lua).requests.load()) {
            if (binding.on_result < 0) { binding.result.reset(); return 0; }
            // Allocation can run close(); retain the result until callback admission.
            const auto result = binding.result;
            return dispatch_callback(lua, binding, binding.on_result, generation, [&](lua_State* lua) {
                push_result(lua, *result); return 1;
            }, [&] {
                binding.result.reset(); return true;
            });
        }
    }
    return 0;
}

template<class Binding, class Push>
int close_action_observer(lua_State* lua, Binding& binding, Push push_result) {
    auto result = binding.connection.stop();
    if (binding.result) result = std::exchange(binding.result, {});
    ++binding.generation;
    binding.closed = true;
    binding.status_pending = binding.change_pending = false;
    unref(lua, binding.on_status); unref(lua, binding.on_change); unref(lua, binding.on_result);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) push_result(lua, *result); else lua_pushnil(lua);
        return 1;
    }, 1);
}

void wake(InvocationState& state) noexcept;

void register_workspaces(lua_State* lua);
int dispatch_workspaces(lua_State* lua, InvocationState& state);
void register_host(lua_State* lua);
int dispatch_utilities(lua_State* lua, InvocationState& state);
void read_clock_change(InvocationState& state);
#ifdef HYPRWORKS_HAS_POLKIT
void register_polkit(lua_State* lua);
int dispatch_polkit(lua_State* lua, PolkitBinding& binding);
#endif
#ifdef HYPRWORKS_HAS_DBUS
void register_dbus(lua_State* lua);
void push_bus_event(lua_State* lua, const dbus::Event& event);
#endif

#ifdef HYPRWORKS_HAS_UPOWER
void register_upower(lua_State* lua);
#endif

#ifdef HYPRWORKS_HAS_AUDIO
void register_audio(lua_State* lua);
#endif

#ifdef HYPRWORKS_HAS_MPRIS
void register_mpris(lua_State* lua);
#endif

#ifdef HYPRWORKS_HAS_NOTIFICATIONS
void register_notifications(lua_State* lua);
void push_notification_event(lua_State* lua, const notifications::Event& event);
#endif

#ifdef HYPRWORKS_HAS_TRAY
void register_tray(lua_State* lua);
int dispatch_menu(lua_State* lua, MenuBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_DESKTOP
void register_desktop(lua_State* lua);
#endif

#ifdef HYPRWORKS_HAS_SHORTCUTS
void register_shortcuts(lua_State* lua);
void push_shortcut_event(lua_State* lua, const shortcuts::Event& event);
#endif

}
