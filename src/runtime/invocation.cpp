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
constexpr std::size_t max_configuration_files = 256;

void watch_configuration_file(InvocationState& state, const std::string& file) {
    const auto path = std::filesystem::absolute(file).lexically_normal();
    const auto resolved = std::filesystem::weakly_canonical(path);
    const std::filesystem::path root(state.configuration_directory);
    if (std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end()).first != root.end()) return;
    auto filename = path.string();
    if (std::ranges::find(state.configuration_files, filename) != state.configuration_files.end()) return;
    if (state.configuration_files.size() == max_configuration_files)
        throw std::runtime_error("configuration file limit reached (256)");
    state.configuration_files.push_back(filename);
    if (auto watched = state.reload_watch.add(filename); !watched) {
        state.configuration_files.pop_back();
        throw std::runtime_error(filename + ": " + watched.error());
    }
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
    const auto code = luaL_optinteger(lua, 1, 0);
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
#ifdef HYPRWORKS_HAS_COMMANDS
    register_commands(lua);
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
    lua_getfield(lua, -1, "searchers");
    lua_rawgeti(lua, -1, 2);
    lua_pushcclosure(lua, guarded<search_configuration_module>, 1);
    lua_rawseti(lua, -2, 2);
    lua_pop(lua, 2);
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
#ifdef HYPRWORKS_HAS_POLKIT
    if (state.polkit) { state.polkit->closed = true; state.polkit->agent.stop(); }
#endif
#ifdef HYPRWORKS_HAS_DBUS
    for (auto& bus : state.buses) { bus->closed = true; bus->connection.stop(); }
#endif
#ifdef HYPRWORKS_HAS_COMMANDS
    if (state.commands) { state.commands->closed = true; state.commands->server.stop(); }
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (state.desktop) { state.desktop->closed = true; state.desktop->entries.stop(); }
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (state.shortcuts) { state.shortcuts->closed = true; state.shortcuts->connection.stop(); }
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (state.tray) { state.tray->closed = true; state.tray->connection.stop(); }
    for (auto& menu : state.menus) { menu->closed = true; menu->connection.stop(); }
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (state.notifications) { state.notifications->closed = true; state.notifications->server.stop(); }
#endif
    for (auto& timer : state.timers) { timer->closed = true; timer->timer.stop(); }
    for (auto& process : state.processes) { process->closed = true; process->process.cancel(); }
    for (auto& watch : state.watches) { watch->closed = true; watch->watch.stop(); }
#ifdef HYPRWORKS_HAS_MPRIS
    if (state.media) { state.media->closed = true; state.media->connection.stop(); }
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (state.audio) {
        state.audio->closed = true;
        state.audio->connection.stop();
    }
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    if (state.power) {
        state.power->closed = true;
        state.power->connection.stop();
    }
#endif
    for (const auto& binding : state.workspaces) {
        binding->closed = true;
        binding->connection.stop();
    }
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
#ifdef HYPRWORKS_HAS_COMMANDS
    state.commands.reset();
#endif
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

std::expected<void, std::string> Invocation::load(const std::string& entry_file, std::span<const std::string> arguments) {
    auto& state = *state_;
    if (state.loaded || state.closing)
        return std::unexpected("an invocation can load only one entry script");
    if (entry_file.empty() || entry_file.contains('\0') || arguments.size() > 4096)
        return std::unexpected("invalid entry path or too many script arguments");
    try {
        const auto path = std::filesystem::absolute(entry_file).lexically_normal();
        const auto filename = path.string();
        state.configuration_directory = std::filesystem::weakly_canonical(path.parent_path()).string();
        state.configuration_files.push_back(filename);
        if (auto watched = state.reload_watch.start(filename); !watched) throw std::runtime_error(watched.error());
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
    const auto append_wait = [&](const auto& prepared) {
        if constexpr (requires { prepared.fd; }) {
            if (prepared.fd.fd >= 0) wait.fds.push_back(prepared.fd);
        } else {
            for (const auto& fd : prepared.fds) if (fd.fd >= 0) wait.fds.push_back(fd);
        }
        if (prepared.deadline && (!wait.deadline || *prepared.deadline < *wait.deadline))
            wait.deadline = prepared.deadline;
    };
#ifdef HYPRWORKS_HAS_DBUS
    for (auto& bus : state.buses) {
        if (bus->closed) continue;
        append_wait(bus->wait = bus->connection.prepare_wait());
        if (bus->status_pending || !bus->events.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
    for (const auto& binding : state.workspaces) {
        if (binding->closed) continue;
        if (binding->status_pending || binding->change_pending || binding->result)
            wait.deadline = native::TimePoint::min();
        append_wait(binding->wait = binding->connection.prepare_wait());
    }
#ifdef HYPRWORKS_HAS_UPOWER
    if (state.power && !state.power->closed) {
        append_wait(state.power->wait = state.power->connection.prepare_wait());
        if (state.power->status_pending || state.power->change_pending)
            wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (state.audio && !state.audio->closed) {
        append_wait(state.audio->wait = state.audio->connection.prepare_wait());
        if (state.audio->status_pending || state.audio->change_pending || state.audio->result) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    if (state.media && !state.media->closed) {
        append_wait(state.media->wait = state.media->connection.prepare_wait());
        if (state.media->status_pending || state.media->change_pending || state.media->result) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (state.notifications && !state.notifications->closed) {
        append_wait(state.notifications->wait = state.notifications->server.prepare_wait());
        if (state.notifications->status_pending || !state.notifications->events.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (state.tray && !state.tray->closed) {
        auto& binding = *state.tray;
        append_wait(binding.wait = binding.connection.prepare_wait());
        if (binding.status_pending || binding.change_pending || binding.result) wait.deadline = native::TimePoint::min();
    }
    for (const auto& menu : state.menus) {
        if (menu->closed) continue;
        append_wait(menu->wait = menu->connection.prepare_wait());
        if (menu->status_pending || menu->change_pending || menu->result || !menu->activations.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (state.shortcuts && !state.shortcuts->closed) {
        auto& binding = *state.shortcuts;
        append_wait(binding.wait = binding.connection.prepare_wait());
        if (binding.status_pending || !binding.events.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (state.desktop && !state.desktop->closed) {
        auto& binding = *state.desktop;
        append_wait(binding.wait = binding.entries.prepare_wait());
        if (binding.status_pending || binding.change_pending) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_COMMANDS
    if (state.commands && !state.commands->closed) {
        auto& binding = *state.commands;
        append_wait(binding.wait = binding.server.prepare_wait());
        if (binding.status_pending || !binding.requests.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    if (state.polkit && !state.polkit->closed) {
        auto& binding = *state.polkit;
        append_wait(binding.wait = binding.agent.prepare_wait());
        if (binding.status_pending || !binding.events.empty()) wait.deadline = native::TimePoint::min();
    }
#endif
    for (const auto& timer : state.timers) {
        if (timer->closed) continue;
        const auto deadline = timer->timer.deadline();
        if (deadline && (!wait.deadline || *deadline < *wait.deadline)) wait.deadline = deadline;
        if (timer->pending) wait.deadline = native::TimePoint::min();
    }
    for (const auto& process : state.processes) {
        if (process->closed) continue;
        if (process->pending) wait.deadline = native::TimePoint::min();
        if (!process->process.running()) continue;
        append_wait(process->wait = process->process.prepare_wait());
    }
    for (const auto& watch : state.watches) {
        if (watch->closed) continue;
        if (watch->pending.changed) wait.deadline = native::TimePoint::min();
        if (!watch->watch.running()) continue;
        watch->wait = watch->watch.prepare_wait();
        wait.fds.push_back(watch->wait);
    }
    if (state.clock_fd >= 0) wait.fds.push_back({state.clock_fd, POLLIN, 0});
    if (state.timezone_watch.running()) wait.fds.push_back(state.timezone_watch.prepare_wait());
    if (state.clock_pending) wait.deadline = native::TimePoint::min();
    if (state.reload_watch.running()) wait.fds.push_back(state.reload_watch.prepare_wait());
    if (state.reload_at && (!wait.deadline || *state.reload_at < *wait.deadline)) wait.deadline = state.reload_at;
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
    std::size_t offset = state.signals ? 2 : 1;
    const auto consume_fd = [&](pollfd& fd) {
        if (fd.fd < 0) { fd.revents = 0; return false; }
        assert(offset < ready.fds.size() && ready.fds[offset].fd == fd.fd && ready.fds[offset].events == fd.events);
        fd.revents = ready.fds[offset++].revents;
        return fd.revents != 0;
    };
    const auto consume_wait = [&](auto& wait) {
        bool due = wait.deadline && *wait.deadline <= now;
        if constexpr (requires { wait.fd; })
            due |= consume_fd(wait.fd);
        else
            for (auto& fd : wait.fds) due |= consume_fd(fd);
        return due;
    };
#ifdef HYPRWORKS_HAS_DBUS
    for (auto& bus : state.buses) {
        if (bus->closed || !consume_wait(bus->wait)) continue;
        auto update = bus->connection.dispatch(bus->wait);
        bus->status_pending |= update.status_changed;
        bus->events = std::move(update.events);
    }
#endif
    for (const auto& binding : state.workspaces) {
        if (binding->closed || !consume_wait(binding->wait))
            continue;
        auto update = binding->connection.dispatch(binding->wait, now);
        binding->status_pending |= update.status_changed;
        binding->change_pending |= update.snapshot_changed;
        if (update.switch_result)
            binding->result = std::move(update.switch_result);
    }
#ifdef HYPRWORKS_HAS_UPOWER
    if (state.power && !state.power->closed && consume_wait(state.power->wait)) {
        const auto update = state.power->connection.dispatch(state.power->wait);
        state.power->status_pending |= update.status_changed;
        state.power->change_pending |= update.snapshot_changed;
    }
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (state.audio && !state.audio->closed && consume_wait(state.audio->wait)) {
        auto update = state.audio->connection.dispatch(state.audio->wait);
        state.audio->status_pending |= update.status_changed;
        state.audio->change_pending |= update.snapshot_changed;
        if (update.result) state.audio->result = std::move(update.result);
    }
#endif

#ifdef HYPRWORKS_HAS_MPRIS
    if (state.media && !state.media->closed && consume_wait(state.media->wait)) {
        auto update = state.media->connection.dispatch(state.media->wait);
        state.media->status_pending |= update.status_changed; state.media->change_pending |= update.snapshot_changed;
        if (update.result) state.media->result = std::move(update.result);
    }
#endif

#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (state.notifications && !state.notifications->closed && consume_wait(state.notifications->wait)) {
        auto update = state.notifications->server.dispatch(state.notifications->wait);
        state.notifications->status_pending |= update.status_changed;
        state.notifications->events = std::move(update.events);
    }
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (state.tray && !state.tray->closed && consume_wait(state.tray->wait)) {
        auto update = state.tray->connection.dispatch(state.tray->wait);
        state.tray->status_pending |= update.status_changed; state.tray->change_pending |= update.snapshot_changed;
        if (update.result) state.tray->result = std::move(update.result);
    }
    for (const auto& menu : state.menus) {
        if (menu->closed || !consume_wait(menu->wait))
            continue;
        auto update = menu->connection.dispatch(menu->wait);
        menu->status_pending |= update.status_changed; menu->change_pending |= update.snapshot_changed;
        if (update.result) menu->result = std::move(update.result);
        menu->activations = std::move(update.activations);
    }
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (state.shortcuts && !state.shortcuts->closed) {
        consume_wait(state.shortcuts->wait);
        auto update = state.shortcuts->connection.dispatch(state.shortcuts->wait);
        state.shortcuts->status_pending |= update.status_changed;
        state.shortcuts->events = std::move(update.events);
    }
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (state.desktop && !state.desktop->closed && consume_wait(state.desktop->wait)) {
        const auto update = state.desktop->entries.dispatch(state.desktop->wait);
        state.desktop->status_pending |= update.status_changed; state.desktop->change_pending |= update.snapshot_changed;
    }
#endif
#ifdef HYPRWORKS_HAS_COMMANDS
    if (state.commands && !state.commands->closed && consume_wait(state.commands->wait)) {
        auto update = state.commands->server.dispatch(state.commands->wait);
        state.commands->status_pending |= update.status_changed; state.commands->requests = std::move(update.requests);
    }
#endif
#ifdef HYPRWORKS_HAS_POLKIT
    if (state.polkit && !state.polkit->closed && consume_wait(state.polkit->wait)) {
        auto update = state.polkit->agent.dispatch(state.polkit->wait);
        state.polkit->status_pending |= update.status_changed;
        state.polkit->events = std::move(update.events);
    }
#endif
    for (auto& timer : state.timers) if (!timer->closed) timer->pending += timer->timer.dispatch(now);
    for (const auto& process : state.processes) {
        if (!process->closed && process->process.running() && consume_wait(process->wait))
            process->pending |= process->process.dispatch(process->wait);
    }
    for (const auto& watch : state.watches) {
        if (!watch->closed && watch->watch.running() && consume_fd(watch->wait)) {
            const auto update = watch->watch.dispatch();
            watch->pending.changed |= update.changed; watch->pending.overflow |= update.overflow; watch->pending.invalidated |= update.invalidated;
        }
    }
    if (state.clock_fd >= 0) {
        pollfd fd{state.clock_fd, POLLIN, 0};
        if (consume_fd(fd)) read_clock_change(state);
    }
    if (state.timezone_watch.running()) {
        auto fd = state.timezone_watch.prepare_wait();
        if (consume_fd(fd)) {
            const auto update = state.timezone_watch.dispatch();
            state.clock_pending |= update.changed;
            if (update.invalidated) {
                const auto watched = state.timezone_watch.start("/etc/localtime");
                if (!watched) throw std::runtime_error(watched.error());
            }
        }
    }
    if (state.reload_watch.running()) {
        auto fd = state.reload_watch.prepare_wait();
        if (consume_fd(fd)) {
            const auto update = state.reload_watch.dispatch();
            if (update.changed) state.reload_at = now + std::chrono::milliseconds(50);
        }
    }
    if (state.reload_at && now >= *state.reload_at) {
        state.reload_at.reset();
        request_reload();
        return;
    }

    int status = dispatch_workspaces(state.lua, state);
#ifdef HYPRWORKS_HAS_DBUS
    const auto bus_count = state.buses.size();
    for (std::size_t i = 0; status >= 0 && i < bus_count; ++i)
        status = dispatch_dbus(state.lua, *state.buses[i]);
#endif
#ifdef HYPRWORKS_HAS_UPOWER
    if (status >= 0 && state.power)
        status = dispatch_power(state.lua, *state.power);
#endif
#ifdef HYPRWORKS_HAS_AUDIO
    if (status >= 0 && state.audio)
        status = dispatch_audio(state.lua, *state.audio);
#endif
#ifdef HYPRWORKS_HAS_MPRIS
    if (status >= 0 && state.media)
        status = dispatch_media(state.lua, *state.media);
#endif
#ifdef HYPRWORKS_HAS_NOTIFICATIONS
    if (status >= 0 && state.notifications)
        status = dispatch_notifications(state.lua, *state.notifications);
#endif
#ifdef HYPRWORKS_HAS_TRAY
    if (status >= 0 && state.tray)
        status = dispatch_tray(state.lua, *state.tray);
    const auto menu_count = state.menus.size();
    for (std::size_t i = 0; status >= 0 && i < menu_count; ++i)
        status = dispatch_menu(state.lua, *state.menus[i]);
#endif
#ifdef HYPRWORKS_HAS_SHORTCUTS
    if (status >= 0 && state.shortcuts)
        status = dispatch_shortcuts(state.lua, *state.shortcuts);
#endif
#ifdef HYPRWORKS_HAS_DESKTOP
    if (status >= 0 && state.desktop)
        status = dispatch_desktop(state.lua, *state.desktop);
#endif
#ifdef HYPRWORKS_HAS_COMMANDS
    if (status >= 0 && state.commands)
        status = dispatch_commands(state.lua, *state.commands);
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
