#include "invocation.hpp"
#ifdef HYPRWORKS_HAS_GUI
#include "../gui/gui.hpp"
#endif

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace hyprworks::lua {
namespace {

constexpr std::size_t max_script_bytes = 4 * 1024 * 1024;

template<class Visit>
void active_connections(InvocationState& state, Visit visit) {
    for (auto& b : state.workspaces) if (b) visit(*b, b->connection);
#ifdef HYPRWORKS_HAS_DBUS
    for (auto& b : state.buses) if (b) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    if (auto& b = state.power; b && !b->closed) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (auto& b = state.audio; b && !b->closed) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    if (auto& b = state.media; b && !b->closed) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (auto& b = state.notifications; b && !b->closed) visit(*b, b->server);
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (auto& b = state.tray; b && !b->closed) visit(*b, b->connection);
    for (auto& b : state.menus) if (b) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (auto& b = state.shortcuts; b && !b->closed) visit(*b, b->connection);
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (auto& b = state.desktop; b && !b->closed) visit(*b, b->entries);
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    if (auto& b = state.polkit; b && !b->closed) visit(*b, b->agent);
#endif
}

template<class Prepared>
auto descriptors(Prepared& wait) {
    if constexpr (requires { wait.fd; }) return std::span(&wait.fd, 1);
    else return std::span(wait.fds);
}

template<class Binding, class Push>
int deliver_events(lua_State* lua, Binding& binding, Push push_event) {
    const auto generation = binding.generation;
    const auto events = std::exchange(binding.events, {});
    if (std::exchange(binding.status_pending, false) &&
        dispatch_callback(lua, binding, binding.on_status, generation, [](lua_State*) { return 0; }) < 0) return -1;
    for (const auto& event : events) {
        if (dispatch_callback(lua, binding, binding.on_event, generation, [&](lua_State* lua) {
            push_event(lua, event); return 1;
        }) < 0) return -1;
    }
    return 0;
}

void watch_configuration_file(InvocationState& state, const std::string& file) {
    const auto path = std::filesystem::absolute(file).lexically_normal();
    const auto resolved = std::filesystem::weakly_canonical(path);
    const std::filesystem::path root(state.configuration_directory);
    if (std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end()).first != root.end()) return;
    if (auto watched = state.reload_watch.add(path.string()); !watched)
        throw std::runtime_error(path.string() + ": " + watched.error());
}

int search_configuration_module(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    lua_settop(lua, 1);
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_pushvalue(lua, 1);
    lua_call(lua, 1, 2);
    if (lua_isfunction(lua, -2) && lua_type(lua, -1) == LUA_TSTRING) {
        std::size_t length;
        const char* filename = lua_tolstring(lua, -1, &length);
        watch_configuration_file(state, std::string(filename, length));
    }
    return 2;
}

int quit(lua_State* lua) {
    const auto code = check_integer(lua, 1);
    luaL_argcheck(lua, code >= 0 && code <= 255, 1, "exit code must be between 0 and 255");
    auto& state = invocation(lua);
    state.exit_code = static_cast<int>(code);
    state.requests.fetch_or(SignalSource::quit, std::memory_order_release);
    return 0;
}

int reload(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    state.requests.fetch_or(SignalSource::reload, std::memory_order_release);
    wake(state);
    return 0;
}

int open_module(lua_State* lua) {
    lua_createtable(lua, 0, 2);
    lua_pushcfunction(lua, guarded<quit>);
    lua_setfield(lua, -2, "quit");
    lua_pushcfunction(lua, guarded<reload>); lua_setfield(lua, -2, "reload");
    register_host(lua);
#ifdef HYPRWORKS_HAS_DBUS
    register_dbus(lua);
#endif
    lua_createtable(lua, 0, 2);
    register_workspaces(lua);
#ifdef HYPRWORKS_HAS_GUI
    register_windows(lua);
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    register_shortcuts(lua);
#endif
    lua_setfield(lua, -2, "hyprland");
#ifdef HYPRWORKS_HAS_UPOWER
    register_upower(lua);
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    register_audio(lua);
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    register_mpris(lua);
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    register_notifications(lua);
#endif
#ifdef HYPRWORKS_HAS_TRAY
    register_tray(lua);
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    register_desktop(lua);
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    register_polkit(lua);
#endif
#ifdef HYPRWORKS_HAS_GUI
    open_gui(lua);
#endif
    return 1;
}

int read_signals(InvocationState& state) noexcept {
    if (!state.signals) return 0;
    for (unsigned i = 0; i < 16; ++i) {
        char wake[256];
        const auto bytes = ::read(state.signals->fd, wake, sizeof(wake));
        if (bytes < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return errno;
        }
        if (!bytes) return EPIPE;
    }
    const auto pending = state.signals->pending.exchange(0, std::memory_order_relaxed);
    state.requests.fetch_or(pending, std::memory_order_release);
    return 0;
}

void interrupt_hook(lua_State* lua, lua_Debug*) {
    auto& state = invocation(lua);
    if (read_signals(state)) luaL_error(lua, "cannot read runtime signal descriptor");
    if (state.closing || state.requests.load(std::memory_order_relaxed)) {
        lua_pushlightuserdata(lua, &interrupted);
        lua_error(lua);
    }
}

int execute_entry(lua_State* lua, const std::string& path, const std::string& module_path,
    const std::string& source, const std::string& chunk_name, std::span<const std::string> arguments) {
    luaL_openlibs(lua);
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, "preload");
    lua_pushcfunction(lua, open_module);
    lua_setfield(lua, -2, "hyprworks");
    lua_pop(lua, 1);
    lua_pushlstring(lua, module_path.data(), module_path.size());
    lua_getfield(lua, -2, "path");
    lua_concat(lua, 2);
    lua_setfield(lua, -2, "path");
    if (invocation(lua).reload_watch.running()) {
        lua_getfield(lua, -1, "searchers");
        lua_rawgeti(lua, -1, 2);
        lua_pushcclosure(lua, guarded<search_configuration_module>, 1);
        lua_rawseti(lua, -2, 2);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    lua_createtable(lua, static_cast<int>(arguments.size()), 1);
    lua_pushlstring(lua, path.data(), path.size());
    lua_rawseti(lua, -2, 0);
    lua_Integer index = 1;
    for (const auto& argument : arguments) {
        lua_pushlstring(lua, argument.data(), argument.size());
        lua_rawseti(lua, -2, index++);
    }
    lua_setglobal(lua, "arg");
    if (luaL_loadbufferx(lua, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK)
        return lua_error(lua);
    lua_call(lua, 0, 0);
    return 0;
}

std::string error_message(lua_State* lua) {
    std::size_t size;
    const char* text = lua_type(lua, -1) == LUA_TSTRING ? lua_tolstring(lua, -1, &size) : nullptr;
    return text ? std::string(text, size) : "unrecognized Lua error";
}

}

InvocationState::InvocationState() = default;
InvocationState::~InvocationState() = default;

void wake(InvocationState& state) noexcept {
    const int saved_errno = errno;
    const std::uint64_t value = 1;
    while (::write(state.wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
    errno = saved_errno;
}

Invocation::Invocation(EventLoop loop, SignalSource* signals) : state_(std::make_unique<InvocationState>()) {
    state_->loop = loop;
    if (signals) {
        const int flags = fcntl(signals->fd, F_GETFL);
        if (flags < 0 || !(flags & O_NONBLOCK)) throw std::invalid_argument("signal descriptor must be nonblocking");
    }
    state_->signals = signals;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static_assert(sizeof(lua_Integer) >= sizeof(std::int64_t));
#ifndef HYPRWORKS_HAS_GUI
    if (loop == EventLoop::gui)
        throw std::runtime_error("GUI support was not enabled at build time");
#endif
    state_->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state_->wake_fd < 0)
        throw std::system_error(errno, std::generic_category(), "invocation wake descriptor");
}

Invocation::~Invocation() {
    close();
    ::close(state_->wake_fd);
}

void Invocation::request_quit() noexcept {
    state_->requests.fetch_or(SignalSource::quit, std::memory_order_release);
    wake(*state_);
}

void Invocation::request_reload() noexcept {
    state_->requests.fetch_or(SignalSource::reload, std::memory_order_release);
    wake(*state_);
}

bool Invocation::reload_requested() const noexcept {
    const auto requests = state_->requests.load(std::memory_order_acquire);
    return (requests & SignalSource::reload) && !(requests & SignalSource::quit);
}

void Invocation::close() noexcept {
    auto& state = *state_;
    if (state.closing) return;
    state.closing = true;
    wake(state);
    state.reload_watch.stop();
    state.reload_at.reset();
    if (state.clock_fd >= 0) ::close(std::exchange(state.clock_fd, -1));
    state.timezone_watch.stop();
    state.clock_pending = false;
    active_connections(state, [](auto& binding, auto& connection) { binding.closed = true; connection.stop(); });
    for (auto& timer : state.timers) if (timer) timer->timer.stop();
    for (auto& process : state.processes) if (process) process->process.cancel();
    for (auto& watch : state.watches) if (watch) watch->watch.stop();
#ifdef HYPRWORKS_HAS_GUI
    if (state.gui) close_gui(*state.gui);
#endif
    if (state.lua)
        lua_close(std::exchange(state.lua, nullptr));
#ifdef HYPRWORKS_HAS_GUI
    state.gui.reset();
#endif
    state.workspaces.clear();
#ifdef HYPRWORKS_HAS_POLKIT
    state.polkit.reset();
#endif
#ifdef HYPRWORKS_HAS_DBUS
    state.buses.clear();
#endif
    state.timers.clear(); state.processes.clear(); state.watches.clear();
#ifdef HYPRWORKS_HAS_DESKTOP
    state.desktop.reset();
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    state.shortcuts.reset();
#endif
#ifdef HYPRWORKS_HAS_TRAY
    state.tray.reset(); state.menus.clear();
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    state.notifications.reset();
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    state.media.reset();
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    state.audio.reset();
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    state.power.reset();
#endif
    state.loaded = false;
}

std::expected<void, std::string> Invocation::load(const std::string& entry_file, std::span<const std::string> arguments, bool watch) {
    auto& state = *state_;
    if (state.loaded || state.closing)
        return std::unexpected("an invocation can load only one entry script");
    if (entry_file.empty() || entry_file.contains('\0') || arguments.size() > 4096)
        return std::unexpected("invalid entry path or too many script arguments");
    try {
        const auto path = std::filesystem::absolute(entry_file).lexically_normal();
        const auto filename = path.string();
        state.configuration_directory = std::filesystem::weakly_canonical(path.parent_path()).string();
        if (watch) {
            if (auto watched = state.reload_watch.start(filename); !watched) throw std::runtime_error(watched.error());
        }
        auto source = host::read_file(filename, max_script_bytes);
        if (!source) throw std::runtime_error(filename + ": " + source.error());
        if (source->starts_with("\xef\xbb\xbf")) source->erase(0, 3);
        if (source->starts_with('#')) {
            const auto end = source->find('\n');
            std::fill_n(source->begin(), end == std::string::npos ? source->size() : end, ' ');
        }
        const auto chunk_name = "@" + filename;
#ifdef HYPRWORKS_HAS_GUI
        if (state.loop == EventLoop::gui) start_gui(state);
#endif
        const auto module_path = path.parent_path().string() + "/?.lua;" + path.parent_path().string() + "/?/init.lua;";
        state.lua = luaL_newstate();
        if (!state.lua) throw std::runtime_error("cannot allocate Lua state");
        *static_cast<InvocationState**>(lua_getextraspace(state.lua)) = &state;
        lua_sethook(state.lua, interrupt_hook, LUA_MASKCOUNT, 10000);
        lua_pushcfunction(state.lua, traceback);
        const int status = protected_call(state.lua, [&](lua_State* lua) {
            return execute_entry(lua, filename, module_path, *source, chunk_name, arguments);
        }, 0, 1);
        if (status < 0 && lua_touserdata(state.lua, -1) != &interrupted)
            throw std::runtime_error(error_message(state.lua));
        lua_settop(state.lua, 0);
        state.loaded = true;
        return {};
    } catch (const std::exception& error) {
        close();
        return std::unexpected(error.what());
    }
}

bool Invocation::running() const noexcept {
    return state_->loaded && !state_->closing && !state_->requests.load(std::memory_order_acquire);
}

int Invocation::exit_code() const noexcept { return state_->exit_code; }

std::expected<void, std::string> Invocation::run() {
    try {
        while (running()) {
            prepare_wait();
            auto& wait = state_->wait;
            timespec timeout{};
            if (wait.deadline) {
                const auto now = std::chrono::steady_clock::now();
                const auto remaining = *wait.deadline <= now ? std::chrono::steady_clock::duration::zero() : *wait.deadline - now;
                const auto ns = std::chrono::ceil<std::chrono::nanoseconds>(remaining);
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ns);
                timeout = {seconds.count(), (ns - seconds).count()};
            }
            if (ppoll(wait.fds.data(), wait.fds.size(), wait.deadline ? &timeout : nullptr, nullptr) < 0) {
                if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "ppoll");
                for (auto& fd : wait.fds) fd.revents = 0;
            }
            dispatch();
        }
        close();
        return {};
    } catch (const std::exception& error) {
        close();
        return std::unexpected(error.what());
    }
}

void Invocation::prepare_wait() {
    auto& state = *state_;
    auto& wait = state.wait;
    wait.fds.clear(); wait.deadline.reset();
    wait.fds.push_back({state.wake_fd, POLLIN, 0});
    if (!running())
        return;
    if (state.signals) wait.fds.push_back({state.signals->fd, POLLIN, 0});
    const auto append_wait = [&](auto& binding, auto& connection) {
        binding.wait = connection.prepare_wait();
        binding.offset = wait.fds.size();
        for (const auto& fd : descriptors(binding.wait)) if (fd.fd >= 0) wait.append(fd);
        wait.due(binding.wait.deadline);
    };
    active_connections(state, [&](auto& binding, auto& connection) {
        append_wait(binding, connection);
        if (binding.pending()) wait.due(native::TimePoint::min());
    });
    for (const auto& timer : state.timers) if (timer) {
        wait.due(timer->timer.deadline());
        if (timer->pending) wait.due(native::TimePoint::min());
    }
    for (const auto& process : state.processes) if (process) {
        if (process->process.running()) append_wait(*process, process->process);
        if (process->pending || process->drained || process->output_pending()) wait.due(native::TimePoint::min());
    }
    for (const auto& watch : state.watches) if (watch) {
        if (watch->watch.running()) watch->offset = wait.append(watch->watch.prepare_wait());
        if (watch->pending.changed) wait.due(native::TimePoint::min());
    }
    if (state.clock_fd >= 0) state.clock_offset = wait.append({state.clock_fd, POLLIN, 0});
    if (state.timezone_watch.running()) state.timezone_offset = wait.append(state.timezone_watch.prepare_wait());
    if (state.clock_pending) wait.due(native::TimePoint::min());
    if (state.reload_watch.running()) state.reload_offset = wait.append(state.reload_watch.prepare_wait());
    wait.due(state.reload_at);
#ifdef HYPRWORKS_HAS_GUI
    if (state.gui) prepare_gui(*state.gui, wait);
#endif
}

void Invocation::dispatch() {
    auto& state = *state_;
    const auto& ready = state.wait;
#ifdef HYPRWORKS_HAS_GUI
    if (state.gui) read_gui(*state.gui, ready);
#endif
    if (ready.fds[0].revents) {
        std::uint64_t wake;
        while (::read(state.wake_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {}
    }
    if (state.signals && ready.fds.size() > 1 && ready.fds[1].revents) {
        if (const int error = read_signals(state))
            throw std::system_error(error, std::generic_category(), "read runtime signals");
    }
    if (!running()) return;
    const auto now = native::Clock::now();
    const auto consume_wait = [&](auto& binding) {
        auto offset = binding.offset;
        bool due = binding.wait.deadline && *binding.wait.deadline <= now;
        for (auto& fd : descriptors(binding.wait)) if (fd.fd >= 0) {
            assert(offset < ready.fds.size() && ready.fds[offset].fd == fd.fd);
            fd.revents = ready.fds[offset++].revents;
            due |= fd.revents != 0;
        }
        return due;
    };
    active_connections(state, [&](auto& binding, auto& connection) {
        const bool due = consume_wait(binding);
#ifdef HYPRWORKS_HAS_SHORTCUTS
        // Borrowed Wayland events arrive through read_gui in this turn.
        if constexpr (std::is_same_v<std::decay_t<decltype(binding)>, ShortcutBinding>)
            binding.retain(connection.dispatch(binding.wait));
        else
#endif
        if (due) binding.retain(connection.dispatch(binding.wait));
    });
    for (auto& timer : state.timers) if (timer) timer->pending += timer->timer.dispatch(now);
    for (auto& process : state.processes)
        if (process && process->process.running() && consume_wait(*process)) {
            const auto queued = process->process.queued();
            process->pending |= process->process.dispatch(process->wait);
            process->drained |= queued && !process->process.queued();
        }
    for (auto& watch : state.watches) {
        if (watch && watch->watch.running() && ready.fds[watch->offset].revents) {
            const auto update = watch->watch.dispatch();
            watch->pending.changed |= update.changed; watch->pending.overflow |= update.overflow; watch->pending.invalidated |= update.invalidated;
        }
    }
    if (state.clock_fd >= 0 && ready.fds[state.clock_offset].revents) read_clock_change(state);
    if (state.timezone_watch.running() && ready.fds[state.timezone_offset].revents) {
        const auto update = state.timezone_watch.dispatch();
        state.clock_pending |= update.changed;
        if (update.invalidated) {
            const auto watched = state.timezone_watch.start("/etc/localtime");
            if (!watched) throw std::runtime_error(watched.error());
        }
    }
    if (state.reload_watch.running() && ready.fds[state.reload_offset].revents) {
        if (state.reload_watch.dispatch().changed) state.reload_at = now + std::chrono::milliseconds(50);
    }
    if (state.reload_at && now >= *state.reload_at) {
        state.reload_at.reset();
        request_reload();
        return;
    }

    int status = dispatch_workspaces(state.lua, state);
#ifdef HYPRWORKS_HAS_DBUS
    for (auto bus : std::span(state.buses)) if (bus && status >= 0) status = deliver_events(state.lua, *bus, push_bus_event);
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    if (status >= 0 && state.power)
        status = deliver_observer(state.lua, *state.power);
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (status >= 0 && state.audio)
        status = deliver_observer(state.lua, *state.audio, push_action_result);
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    if (status >= 0 && state.media)
        status = deliver_observer(state.lua, *state.media, push_action_result);
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (status >= 0 && state.notifications)
        status = deliver_events(state.lua, *state.notifications, push_notification_event);
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (status >= 0 && state.tray)
        status = deliver_observer(state.lua, *state.tray, push_action_result);
    for (auto menu : std::span(state.menus)) if (menu && status >= 0) status = dispatch_menu(state.lua, *menu);
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (status >= 0 && state.shortcuts)
        status = deliver_events(state.lua, *state.shortcuts, push_shortcut_event);
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (status >= 0 && state.desktop)
        status = deliver_observer(state.lua, *state.desktop);
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    if (status >= 0 && state.polkit)
        status = dispatch_polkit(state.lua, *state.polkit);
#endif
    if (status >= 0)
        status = dispatch_utilities(state.lua, state);
#ifdef HYPRWORKS_HAS_GUI
    if (status >= 0 && state.gui && !state.requests.load(std::memory_order_relaxed))
        status = dispatch_gui(*state.gui);
#endif
    if (status < 0 && lua_touserdata(state.lua, -1) != &interrupted)
        throw std::runtime_error(error_message(state.lua));
}

}
