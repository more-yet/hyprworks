#include "../runtime/invocation.hpp"

#include <utility>
#include <cerrno>
#include <ctime>
#include <limits>
#include <system_error>
#include <sys/timerfd.h>
#include <unistd.h>

namespace hyprworks::lua {
namespace {

constexpr char timer_metatable[] = "hyprworks.timer";
constexpr char process_metatable[] = "hyprworks.process";
constexpr char watch_metatable[] = "hyprworks.watch";

void arm_clock_change(int fd) {
    itimerspec timer{};
    timer.it_value.tv_sec = std::numeric_limits<time_t>::max();
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &timer, nullptr) < 0)
        throw std::system_error(errno, std::generic_category(), "arm clock change notification");
}

int on_clock_change(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!lua_isnoneornil(lua, 1)) luaL_checktype(lua, 1, LUA_TFUNCTION);
    state.clock_pending = replace_callback(lua, 1, state.on_clock_change, state.closing);
    ++state.clock_generation;
    if (state.on_clock_change < 0) {
        if (state.clock_fd >= 0) close(std::exchange(state.clock_fd, -1));
        state.timezone_watch.stop();
    } else if (state.clock_fd < 0) {
        state.clock_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        try {
            if (state.clock_fd < 0) throw std::system_error(errno, std::generic_category(), "watch clock changes");
            arm_clock_change(state.clock_fd);
            const auto watched = state.timezone_watch.start("/etc/localtime");
            if (!watched) throw std::runtime_error(watched.error());
        } catch (...) {
            if (state.clock_fd >= 0) close(std::exchange(state.clock_fd, -1));
            state.timezone_watch.stop();
            unref(lua, state.on_clock_change); state.clock_pending = false;
            throw;
        }
    }
    lua_pushboolean(lua, true);
    return 1;
}

TimerBinding& timer_handle(lua_State* lua) {
    auto& binding = **static_cast<TimerBinding**>(luaL_checkudata(lua, 1, timer_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

ProcessBinding& process_handle(lua_State* lua) {
    auto& binding = **static_cast<ProcessBinding**>(luaL_checkudata(lua, 1, process_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

WatchBinding& watch_handle(lua_State* lua) {
    auto& binding = **static_cast<WatchBinding**>(luaL_checkudata(lua, 1, watch_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

int timer_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    const auto milliseconds = luaL_checkinteger(lua, 1);
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    const bool repeat = lua_toboolean(lua, 3);
    if (state.timers.size() == 128) return luaL_error(lua, "timer handle limit reached; reuse existing timers");
    auto timer = std::make_unique<TimerBinding>();
    if (auto result = timer->timer.start(std::chrono::milliseconds(milliseconds), repeat); !result) throw std::runtime_error(result.error());
    auto& binding = *timer;
    state.timers.push_back(std::move(timer));
    lua_pushvalue(lua, 2);
    const int status = protected_call(lua, [&](lua_State* lua) {
        *static_cast<TimerBinding**>(lua_newuserdatauv(lua, sizeof(TimerBinding*), 0)) = &binding;
        luaL_setmetatable(lua, timer_metatable);
        lua_pushvalue(lua, -1); binding.handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        replace_callback(lua, 2, binding.callback, binding.closed);
        return 1;
    }, 1, 0, 1);
    if (status < 0) { binding.closed = true; binding.timer.stop(); }
    return status;
}

int timer_start(lua_State* lua) {
    auto& binding = timer_handle(lua);
    const auto milliseconds = luaL_checkinteger(lua, 2);
    auto result = binding.timer.start(std::chrono::milliseconds(milliseconds), lua_toboolean(lua, 3));
    if (!result) throw std::runtime_error(result.error());
    binding.closed = false; binding.pending = 0; ++binding.generation;
    return 0;
}

int timer_stop(lua_State* lua) {
    auto& binding = timer_handle(lua);
    binding.timer.stop(); binding.pending = 0; ++binding.generation;
    return 0;
}

int timer_close(lua_State* lua) {
    auto& binding = timer_handle(lua);
    binding.timer.stop(); binding.pending = 0; ++binding.generation;
    binding.closed = true;
    unref(lua, binding.callback);
    return 0;
}

int timer_callback(lua_State* lua) {
    auto& binding = timer_handle(lua);
    replace_callback(lua, 2, binding.callback, binding.closed); return 0;
}

void parse_arguments(lua_State* lua, int argument, std::vector<std::string>& arguments) {
    luaL_checktype(lua, argument, LUA_TTABLE);
    const auto size = lua_rawlen(lua, argument);
    luaL_argcheck(lua, size > 0 && size <= 256, argument, "expected 1..256 process arguments");
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < size; ++i) {
        lua_rawgeti(lua, argument, static_cast<lua_Integer>(i + 1));
        luaL_checktype(lua, -1, LUA_TSTRING);
        std::size_t length;
        const char* value = lua_tolstring(lua, -1, &length);
        luaL_argcheck(lua, length <= 65536 && (bytes += length + 1) <= 1024 * 1024, argument, "process argument budget exceeded");
        arguments.emplace_back(value, length);
        lua_pop(lua, 1);
    }
}

void parse_process_options(lua_State* lua, int argument, host::ProcessOptions& options) {
    if (lua_isnoneornil(lua, argument)) return;
    luaL_checktype(lua, argument, LUA_TTABLE);
    lua_getfield(lua, argument, "directory");
    if (!lua_isnil(lua, -1)) {
        std::size_t length;
        const char* value = luaL_checklstring(lua, -1, &length);
        luaL_argcheck(lua, length <= 16384, argument, "directory exceeds 16 KiB");
        options.directory.assign(value, length);
    }
    lua_pop(lua, 1);
    lua_getfield(lua, argument, "timeout");
    if (!lua_isnil(lua, -1)) options.timeout = std::chrono::milliseconds(luaL_checkinteger(lua, -1));
    lua_pop(lua, 1);
    lua_getfield(lua, argument, "output_limit");
    if (!lua_isnil(lua, -1)) {
        const auto limit = luaL_checkinteger(lua, -1);
        luaL_argcheck(lua, limit >= 0 && limit <= 16 * 1024 * 1024, argument, "output limit must be 0..16 MiB per stream");
        options.output_limit = static_cast<std::size_t>(limit);
    }
    lua_pop(lua, 1);
}

void push_process_result(lua_State* lua, const std::optional<host::ProcessResult>& result) {
    if (!result) { lua_pushnil(lua); return; }
    static const char* outcomes[] = {"exited", "signalled", "cancelled", "timed_out", "failed"};
    lua_createtable(lua, 0, 7);
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result->outcome)]); lua_setfield(lua, -2, "outcome");
    lua_pushinteger(lua, result->exit_code); lua_setfield(lua, -2, "exit_code");
    lua_pushinteger(lua, result->signal); lua_setfield(lua, -2, "signal");
    lua_pushboolean(lua, result->truncated); lua_setfield(lua, -2, "truncated");
    string_field(lua, "stdout", result->stdout_text); string_field(lua, "stderr", result->stderr_text);
    if (!result->error.empty()) string_field(lua, "error", result->error);
}

int process_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    std::vector<std::string> arguments;
    host::ProcessOptions options;
    lua_settop(lua, 2);
    lua_pushvalue(lua, 1);
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) { parse_arguments(lua, 2, arguments); parse_process_options(lua, 3, options); return 0; }, 0, 0, 2) < 0) return -1;
    if (state.processes.size() >= 64) throw std::runtime_error("process handle limit reached; reuse existing handles");
    auto created = std::make_unique<ProcessBinding>();
    auto& binding = *created;
    const auto result = binding.process.start(arguments, options);
    if (!result) return operation_result(lua, result);
    state.processes.push_back(std::move(created));
    const int status = retain_handle(lua, binding, process_metatable);
    if (status < 0) { binding.closed = true; binding.process.cancel(); }
    return status;
}

int process_start(lua_State* lua) {
    auto& binding = process_handle(lua);
    std::vector<std::string> arguments;
    host::ProcessOptions options;
    lua_settop(lua, 3);
    lua_pushvalue(lua, 2);
    lua_pushvalue(lua, 3);
    if (protected_call(lua, [&](lua_State* lua) { parse_arguments(lua, 2, arguments); parse_process_options(lua, 3, options); return 0; }, 0, 0, 2) < 0) return -1;
    const auto result = binding.process.start(arguments, options);
    if (result) { binding.closed = false; binding.pending = false; ++binding.generation; }
    return operation_result(lua, result);
}

int process_result(lua_State* lua) {
    const auto result = process_handle(lua).process.result();
    return protected_call(lua, [&](lua_State* lua) { push_process_result(lua, result); return 1; }, 1);
}

int process_running(lua_State* lua) {
    lua_pushboolean(lua, process_handle(lua).process.running()); return 1;
}

int process_callback(lua_State* lua) {
    auto& binding = process_handle(lua);
    replace_callback(lua, 2, binding.callback, binding.closed);
    binding.pending = binding.callback >= 0 && binding.process.result().has_value();
    return 0;
}

int process_close(lua_State* lua) {
    auto& binding = process_handle(lua);
    binding.process.cancel(); binding.closed = true; binding.pending = false; ++binding.generation;
    unref(lua, binding.callback);
    return process_result(lua);
}

int launch_application(lua_State* lua) {
    if (invocation(lua).closing) return luaL_error(lua, "invocation is shutting down");
    std::vector<std::string> arguments;
    std::string directory;
    const bool has_directory = !lua_isnone(lua, 2);
    lua_pushvalue(lua, 1);
    if (!has_directory) lua_pushnil(lua); else lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        parse_arguments(lua, 2, arguments);
        if (!lua_isnil(lua, 3)) {
            std::size_t length;
            const char* value = luaL_checklstring(lua, 3, &length);
            luaL_argcheck(lua, length <= 16384, 3, "directory exceeds 16 KiB");
            directory.assign(value, length);
        }
        return 0;
    }, 0, 0, 2) < 0) return -1;
    const auto result = host::launch(arguments, directory);
    return operation_result(lua, result);
}

int read_file(lua_State* lua) {
    if (invocation(lua).closing) return luaL_error(lua, "invocation is shutting down");
    std::size_t path_length;
    const char* path = luaL_checklstring(lua, 1, &path_length);
    luaL_argcheck(lua, path_length <= 16384, 1, "path exceeds 16 KiB");
    const auto limit = luaL_optinteger(lua, 2, 1024 * 1024);
    luaL_argcheck(lua, limit >= 0 && limit <= 16 * 1024 * 1024, 2, "read limit must be 0..16 MiB");
    const auto result = host::read_file(std::string(path, path_length), static_cast<std::size_t>(limit));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushlstring(lua, result->data(), result->size()); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int write_file(lua_State* lua) {
    if (invocation(lua).closing) return luaL_error(lua, "invocation is shutting down");
    std::size_t path_length, length;
    const char* path = luaL_checklstring(lua, 1, &path_length);
    luaL_argcheck(lua, path_length <= 16384, 1, "path exceeds 16 KiB");
    const char* contents = luaL_checklstring(lua, 2, &length);
    luaL_argcheck(lua, length <= 16 * 1024 * 1024, 2, "write exceeds 16 MiB");
    const auto result = host::write_file(std::string(path, path_length), std::string(contents, length));
    return operation_result(lua, result);
}

int watch_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    std::size_t length;
    const char* path = luaL_checklstring(lua, 1, &length);
    luaL_argcheck(lua, length <= 16384, 1, "path exceeds 16 KiB");
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    const bool directory = lua_toboolean(lua, 3);
    if (state.watches.size() == 64) return luaL_error(lua, "file watch handle limit reached");
    auto watch = std::make_unique<WatchBinding>();
    const auto result = watch->watch.start(std::string(path, length), directory);
    if (!result) return operation_result(lua, result);
    auto& binding = *watch;
    state.watches.push_back(std::move(watch));
    lua_pushvalue(lua, 2);
    const int status = protected_call(lua, [&](lua_State* lua) {
        *static_cast<WatchBinding**>(lua_newuserdatauv(lua, sizeof(WatchBinding*), 0)) = &binding;
        luaL_setmetatable(lua, watch_metatable);
        lua_pushvalue(lua, -1); binding.handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        replace_callback(lua, 2, binding.callback, binding.closed); return 1;
    }, 1, 0, 1);
    if (status < 0) { binding.closed = true; binding.watch.stop(); }
    return status;
}

int watch_close(lua_State* lua) {
    auto& binding = watch_handle(lua);
    binding.closed = true; binding.watch.stop(); binding.pending = {}; ++binding.generation;
    unref(lua, binding.callback); return 0;
}

int watch_start(lua_State* lua) {
    auto& binding = watch_handle(lua);
    std::size_t length;
    const char* path = luaL_checklstring(lua, 2, &length);
    luaL_argcheck(lua, length <= 16384, 2, "path exceeds 16 KiB");
    const auto result = binding.watch.start(std::string(path, length), lua_toboolean(lua, 3));
    binding.closed = false; binding.pending = {}; ++binding.generation;
    return operation_result(lua, result);
}

int watch_callback(lua_State* lua) {
    auto& binding = watch_handle(lua);
    replace_callback(lua, 2, binding.callback, binding.closed); return 0;
}

}

void read_clock_change(InvocationState& state) {
    std::uint64_t ticks;
    ssize_t count;
    do { count = read(state.clock_fd, &ticks, sizeof(ticks)); } while (count < 0 && errno == EINTR);
    if (count < 0 && errno == EAGAIN) return;
    if (count < 0 && errno != ECANCELED)
        throw std::system_error(errno, std::generic_category(), "read clock change notification");
    arm_clock_change(state.clock_fd);
    state.clock_pending = true;
}

int dispatch_utilities(lua_State* lua, InvocationState& state) {
    if (std::exchange(state.clock_pending, false) && state.on_clock_change >= 0 &&
        !state.requests.load(std::memory_order_relaxed)) {
        const int reference = state.on_clock_change;
        const auto generation = state.clock_generation;
        tzset();
        lua_pushcfunction(lua, traceback);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
            if (state.on_clock_change == reference && state.clock_generation == generation) lua_call(lua, 0, 0);
            return 0;
        }, 0, 1) < 0) return -1;
        lua_settop(lua, 0);
    }
    const auto timers = state.timers.size(), processes = state.processes.size(), watches = state.watches.size();
    for (std::size_t i = 0; i < timers; ++i) {
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *state.timers[i];
        if (binding.closed || !binding.pending) continue;
        const auto ticks = std::exchange(binding.pending, 0);
        const auto generation = binding.generation;
        if (dispatch_callback(lua, binding, binding.callback, generation, [&](lua_State* lua) {
            lua_pushinteger(lua, static_cast<lua_Integer>(ticks));
            return 1;
        }) < 0) return -1;
    }
    for (std::size_t i = 0; i < processes; ++i) {
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *state.processes[i];
        if (binding.closed || !binding.pending) continue;
        binding.pending = false;
        if (binding.callback < 0) continue;
        const auto generation = binding.generation;
        const auto result = binding.process.result();
        if (dispatch_callback(lua, binding, binding.callback, generation, [&](lua_State* lua) {
            push_process_result(lua, result);
            return 1;
        }) < 0) return -1;
    }
    for (std::size_t i = 0; i < watches; ++i) {
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *state.watches[i];
        if (binding.closed || !binding.pending.changed) continue;
        const auto event = std::exchange(binding.pending, {});
        const auto generation = binding.generation;
        if (dispatch_callback(lua, binding, binding.callback, generation, [&](lua_State* lua) {
            lua_createtable(lua, 0, 2);
            lua_pushboolean(lua, event.overflow); lua_setfield(lua, -2, "overflow");
            lua_pushboolean(lua, event.invalidated); lua_setfield(lua, -2, "invalidated");
            return 1;
        }) < 0) return -1;
    }
    return 0;
}

void register_host(lua_State* lua) {
    lua_pushcfunction(lua, operation_call<on_clock_change>); lua_setfield(lua, -2, "on_clock_change");
    lua_pushcfunction(lua, guarded<timer_open>); lua_setfield(lua, -2, "timer");
    lua_pushcfunction(lua, guarded<process_open>); lua_setfield(lua, -2, "process");
    lua_pushcfunction(lua, guarded<launch_application>); lua_setfield(lua, -2, "launch");
    lua_pushcfunction(lua, guarded<read_file>); lua_setfield(lua, -2, "read_file");
    lua_pushcfunction(lua, guarded<write_file>); lua_setfield(lua, -2, "write_file");
    lua_pushcfunction(lua, guarded<watch_open>); lua_setfield(lua, -2, "watch_file");
    const luaL_Reg timer_methods[] = {{"start", guarded<timer_start>}, {"stop", guarded<timer_stop>},
        {"close", guarded<timer_close>}, {"on_tick", guarded<timer_callback>}, {nullptr, nullptr}};
    const luaL_Reg process_methods[] = {{"start", guarded<process_start>}, {"close", guarded<process_close>},
        {"running", guarded<process_running>}, {"result", guarded<process_result>}, {"on_exit", guarded<process_callback>}, {nullptr, nullptr}};
    const luaL_Reg watch_methods[] = {{"close", guarded<watch_close>}, {"start", guarded<watch_start>}, {"on_change", guarded<watch_callback>}, {nullptr, nullptr}};
    const struct { const char* name; const luaL_Reg* methods; lua_CFunction close; } metatables[] = {
        {timer_metatable, timer_methods, guarded<timer_close>},
        {process_metatable, process_methods, guarded<process_close>},
        {watch_metatable, watch_methods, guarded<watch_close>}
    };
    for (const auto& metatable : metatables) {
        luaL_newmetatable(lua, metatable.name);
        luaL_setfuncs(lua, metatable.methods, 0);
        lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
        lua_pushcfunction(lua, metatable.close); lua_setfield(lua, -2, "__close");
        lua_pushliteral(lua, "hyprworks host utility"); lua_setfield(lua, -2, "__metatable");
        lua_pop(lua, 1);
    }
}

}
