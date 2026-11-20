#pragma once

#include <hyprworks/lua.hpp>
#include <hyprworks/hyprland/workspaces.hpp>
#include <hyprworks/host.hpp>
#ifdef HYPRWORKS_HAS_DBUS
#include <hyprworks/dbus.hpp>
#endif
#ifdef HYPRWORKS_HAS_COMMANDS
#include <hyprworks/commands.hpp>
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

#include "binding.hpp"
#include <array>
#include <atomic>

namespace hyprworks::lua {

namespace native = hyprworks::hyprland;
struct Gui;
struct WaitSet {
    std::vector<pollfd> fds;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct TimerBinding {
    host::Timer timer;
    int handle = LUA_NOREF, callback = LUA_NOREF;
    std::uint64_t pending = 0, generation = 0;
    bool closed = false;
};

struct ProcessBinding {
    host::Process process;
    host::ProcessWait wait;
    int handle = LUA_NOREF, callback = LUA_NOREF;
    std::uint64_t generation = 0;
    bool pending = false, closed = false;
};

struct WatchBinding {
    host::FileWatch watch;
    pollfd wait;
    int handle = LUA_NOREF, callback = LUA_NOREF;
    host::FileUpdate pending;
    std::uint64_t generation = 0;
    bool closed = false;
};

#ifdef HYPRWORKS_HAS_COMMANDS
struct CommandBinding {
    commands::CommandServer server;
    commands::WaitSet wait;
    std::string name;
    std::vector<std::string> commands;
    std::vector<commands::Request> requests;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_request = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, status_pending = false;
};
#endif

#ifdef HYPRWORKS_HAS_DESKTOP
struct DesktopBinding {
    desktop::DesktopEntries entries;
    desktop::WaitSet wait;
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
    std::string app_id, display;
    std::vector<shortcuts::Shortcut> shortcuts;
    std::vector<shortcuts::Event> events;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_event = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, status_pending = false;
};
#endif

enum class Event { status, change, result };

struct WorkspaceBinding {
    struct Listener {
        lua_Integer id;
        Event event;
        int reference = LUA_NOREF;
        bool initial = false;
    };
    native::SessionTarget target;
    native::WorkspaceConnection connection;
    native::WaitSet wait;
    int handle = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false;
    bool status_changed = false;
    bool snapshot_changed = false;
    std::optional<native::SwitchResult> result;
    std::vector<Listener> listeners;
    lua_Integer last_listener_id = 0;
};

#ifdef HYPRWORKS_HAS_TRAY
struct TrayBinding {
    tray::TrayConnection connection;
    tray::WaitSet wait;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, own_watcher = false, status_pending = false, change_pending = false;
    std::uint64_t generation = 0;
    std::optional<tray::RequestResult> result;
};
struct MenuBinding {
    dbusmenu::MenuConnection connection;
    dbusmenu::WaitSet wait;
    dbusmenu::Target target;
    int handle = LUA_NOREF, on_status = LUA_NOREF, on_change = LUA_NOREF, on_result = LUA_NOREF, on_activation = LUA_NOREF;
    bool closed = false, status_pending = false, change_pending = false;
    std::uint64_t generation = 0;
    std::optional<dbusmenu::RequestResult> result;
    std::vector<dbusmenu::Activation> activations;
};
#endif

#ifdef HYPRWORKS_HAS_NOTIFICATIONS
struct NotificationBinding {
    notifications::NotificationServer server;
    notifications::WaitSet wait;
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
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, change_pending = false, status_pending = false;
    std::uint64_t generation = 0;
    std::optional<mpris::RequestResult> result;
};
#endif

#ifdef HYPRWORKS_HAS_AUDIO
struct AudioBinding {
    audio::AudioConnection connection;
    audio::WaitSet wait;
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF, on_result = LUA_NOREF;
    bool closed = false, change_pending = false, status_pending = false;
    std::uint64_t generation = 0;
    std::optional<audio::RequestResult> result;
};
#endif

#ifdef HYPRWORKS_HAS_UPOWER
struct PowerBinding {
    upower::PowerConnection connection;
    upower::WaitSet wait;
    int handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF;
    std::uint64_t generation = 0;
    bool closed = false, change_pending = false, status_pending = false;
};
#endif

struct InvocationState {
    InvocationState();
    ~InvocationState();
    lua_State* lua = nullptr;
    EventLoop loop = EventLoop::headless;
    std::atomic<bool> quit_requested{false};
    std::atomic<bool> reload_requested{false};
    int wake_fd = -1;
    SignalSource* signals = nullptr;
    bool loaded = false;
    bool closing = false;
    int exit_code = 0;
    WaitSet wait;
    std::vector<std::unique_ptr<WorkspaceBinding>> workspaces;
    std::vector<std::unique_ptr<TimerBinding>> timers;
    std::vector<std::unique_ptr<ProcessBinding>> processes;
    std::vector<std::unique_ptr<WatchBinding>> watches;
#ifdef HYPRWORKS_HAS_DBUS
    struct BusBinding {
        dbus::Connection connection;
        dbus::WaitSet wait;
        std::vector<dbus::Event> events;
        int handle = LUA_NOREF, on_event = LUA_NOREF, on_status = LUA_NOREF;
        std::uint64_t generation = 0;
        bool closed = false, status_pending = true;
    };
    std::vector<std::unique_ptr<BusBinding>> buses;
#endif
    host::FileWatch reload_watch;
    std::optional<host::Clock::time_point> reload_at;
#ifdef HYPRWORKS_HAS_COMMANDS
    std::optional<CommandBinding> commands;
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    std::optional<DesktopBinding> desktop;
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    std::optional<ShortcutBinding> shortcuts;
#endif
#ifdef HYPRWORKS_HAS_TRAY
    std::optional<TrayBinding> tray;
    std::vector<std::unique_ptr<MenuBinding>> menus;
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
};

inline InvocationState& invocation(lua_State* lua) {
    return **static_cast<InvocationState**>(lua_getextraspace(lua));
}

void wake(InvocationState& state) noexcept;

bool active(const WorkspaceBinding& binding);
bool pending(const WorkspaceBinding& binding);
void register_workspaces(lua_State* lua);
int dispatch_workspaces(lua_State* lua, InvocationState& state);
void register_host(lua_State* lua);
int dispatch_utilities(lua_State* lua, InvocationState& state);
#ifdef HYPRWORKS_HAS_DBUS
void register_dbus(lua_State* lua);
int dispatch_dbus(lua_State* lua, InvocationState::BusBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_UPOWER
void register_upower(lua_State* lua);
int dispatch_power(lua_State* lua, PowerBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_AUDIO
void register_audio(lua_State* lua);
int dispatch_audio(lua_State* lua, AudioBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_MPRIS
void register_mpris(lua_State* lua);
int dispatch_media(lua_State* lua, MediaBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_NOTIFICATIONS
void register_notifications(lua_State* lua);
int dispatch_notifications(lua_State* lua, NotificationBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_TRAY
void register_tray(lua_State* lua);
int dispatch_tray(lua_State* lua, TrayBinding& binding);
int dispatch_menu(lua_State* lua, MenuBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_COMMANDS
void register_commands(lua_State* lua);
int dispatch_commands(lua_State* lua, CommandBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_DESKTOP
void register_desktop(lua_State* lua);
int dispatch_desktop(lua_State* lua, DesktopBinding& binding);
#endif

#ifdef HYPRWORKS_HAS_SHORTCUTS
void register_shortcuts(lua_State* lua);
int dispatch_shortcuts(lua_State* lua, ShortcutBinding& binding);
#endif

}
