#include "../runtime/invocation.hpp"

#include <utility>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <filesystem>
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
    state.clock_pending = replace_callback(lua, 1, state.on_clock_change, state.closing);
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
    auto& binding = **static_cast<std::shared_ptr<TimerBinding>*>(luaL_checkudata(lua, 1, timer_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

ProcessBinding& process_handle(lua_State* lua) {
    auto& binding = **static_cast<std::shared_ptr<ProcessBinding>*>(luaL_checkudata(lua, 1, process_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

WatchBinding& watch_handle(lua_State* lua) {
    auto& binding = **static_cast<std::shared_ptr<WatchBinding>*>(luaL_checkudata(lua, 1, watch_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    return binding;
}

int timer_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    const auto milliseconds = check_integer(lua, 1);
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool repeat = lua_toboolean(lua, 3);
    auto& slot = vacant_slot(state.timers);
    auto timer = std::make_shared<TimerBinding>();
    if (auto result = timer->timer.start(std::chrono::milliseconds(milliseconds), repeat); !result) throw std::runtime_error(result.error());
    return open_active(lua, slot, timer, timer_metatable, &timer->callback);
}

int timer_start(lua_State* lua) {
    auto& binding = timer_handle(lua);
    const auto milliseconds = check_integer(lua, 2);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool closed = binding.closed;
    if (resume_handle(lua, invocation(lua).timers, binding) < 0) return -1;
    auto result = binding.timer.start(std::chrono::milliseconds(milliseconds), lua_toboolean(lua, 3));
    if (!result) {
        if (closed) untrack(lua, invocation(lua).timers, binding);
        throw std::runtime_error(result.error());
    }
    binding.pending = 0; ++binding.generation;
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
    untrack(lua, invocation(lua).timers, binding);
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
    luaL_checktype(lua, argument, LUA_TTABLE);
    field(lua, argument, "directory");
    if (!lua_isnil(lua, -1)) {
        std::size_t length;
        const char* value = check_string(lua, -1, &length);
        luaL_argcheck(lua, length <= 16384, argument, "directory exceeds 16 KiB");
        options.directory.assign(value, length);
    }
    lua_pop(lua, 1);
    field(lua, argument, "timeout");
    options.timeout = std::chrono::milliseconds(check_integer(lua, -1));
    lua_pop(lua, 1);
    field(lua, argument, "output_limit");
    const auto limit = check_integer(lua, -1);
    luaL_argcheck(lua, limit >= 0 && limit <= 16 * 1024 * 1024, argument, "output limit must be 0..16 MiB per stream");
    options.output_limit = static_cast<std::size_t>(limit);
    lua_pop(lua, 1);
    options.input_limit = static_cast<std::size_t>(integer_field(lua, argument, "input_limit", 65536, 1, 1024 * 1024));
    field(lua, argument, "pty");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        auto& term = options.terminal.emplace();
        term.rows = static_cast<unsigned short>(integer_field(lua, -1, "rows", term.rows, 1, 65535));
        term.columns = static_cast<unsigned short>(integer_field(lua, -1, "columns", term.columns, 1, 65535));
        term.raw = boolean_field(lua, -1, "raw", false);
        term.echo = boolean_field(lua, -1, "echo", !term.raw);
    } else {
        text_field(lua, argument, "stdin_file", options.stdin_file);
        text_field(lua, argument, "stdout_file", options.stdout_file);
        text_field(lua, argument, "stderr_file", options.stderr_file);
    }
    lua_pop(lua, 1);
    field(lua, argument, "env");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        std::size_t bytes = 0;
        lua_pushnil(lua);
        while (lua_next(lua, -2)) {
            std::size_t k, v;
            const char* key = check_string(lua, -2, &k);
            const char* value = check_string(lua, -1, &v);
            if (options.environment.size() == 128 || k > 255 || v > 65536 || (bytes += k + v + 2) > 1024 * 1024)
                luaL_error(lua, "process environment exceeds budget");
            options.environment.emplace_back(std::string(key, k), std::string(value, v)); lua_pop(lua, 1);
        }
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
    lua_pushinteger(lua, static_cast<lua_Integer>(result->stdin_discarded)); lua_setfield(lua, -2, "stdin_discarded");
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
    auto& slot = vacant_slot(state.processes);
    auto binding = std::make_shared<ProcessBinding>();
    const auto result = binding->process.start(arguments, options);
    if (!result) return operation_result(lua, result);
    return open_active(lua, slot, binding, process_metatable);
}

int process_start(lua_State* lua) {
    auto& binding = process_handle(lua);
    const auto generation = binding.generation;
    std::vector<std::string> arguments;
    host::ProcessOptions options;
    lua_settop(lua, 3);
    lua_pushvalue(lua, 2);
    lua_pushvalue(lua, 3);
    if (protected_call(lua, [&](lua_State* lua) { parse_arguments(lua, 2, arguments); parse_process_options(lua, 3, options); return 0; }, 0, 0, 2) < 0) return -1;
    if (binding.generation != generation) throw std::runtime_error("process changed while parsing options");
    const bool closed = binding.closed;
    if (resume_handle(lua, invocation(lua).processes, binding) < 0) return -1;
    const auto result = binding.process.start(arguments, options);
    if (result) { binding.pending = binding.drained = false; binding.delivered = {}; ++binding.generation; }
    else if (closed) untrack(lua, invocation(lua).processes, binding);
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
    replace_callback(lua, 2, binding.on_exit, binding.closed);
    binding.pending = binding.on_exit >= 0 && binding.process.result().has_value();
    return 0;
}

int process_close(lua_State* lua) {
    auto& binding = process_handle(lua);
    binding.process.cancel(); untrack(lua, invocation(lua).processes, binding); binding.pending = false; ++binding.generation;
    unref(lua, binding.on_exit); unref(lua, binding.on_output); unref(lua, binding.on_drain);
    const auto result = binding.process.result();
    return protected_call(lua, [&](lua_State* lua) { push_process_result(lua, result); return 1; }, 1);
}

int on_output(lua_State* lua) {
    auto& binding = process_handle(lua);
    replace_callback(lua, 2, binding.on_output, binding.closed); return 0;
}
int on_drain(lua_State* lua) {
    auto& binding = process_handle(lua);
    replace_callback(lua, 2, binding.on_drain, binding.closed); return 0;
}
int process_write(lua_State* lua) {
    std::size_t n; const char* data = check_string(lua, 2, &n);
    const auto result = process_handle(lua).process.write({data, n});
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushinteger(lua, static_cast<lua_Integer>(*result)); return 1; }
        lua_pushnil(lua); lua_pushstring(lua, result.error().c_str()); return 2;
    }, 2);
}
int close_input(lua_State* lua) { return operation_result(lua, process_handle(lua).process.close_input()); }
int process_hangup(lua_State* lua) {
    auto& binding = process_handle(lua);
    const auto queued = binding.process.queued();
    const auto result = binding.process.hangup();
    if (result && queued) binding.drained = true;
    return operation_result(lua, result);
}
int process_resize(lua_State* lua) {
    const auto rows = check_integer(lua, 2), columns = check_integer(lua, 3);
    luaL_argcheck(lua, rows > 0 && rows <= 65535 && columns > 0 && columns <= 65535, 2, "invalid terminal dimensions");
    return operation_result(lua, process_handle(lua).process.resize(static_cast<unsigned short>(rows), static_cast<unsigned short>(columns)));
}
int process_signal(lua_State* lua) {
    const auto n = check_integer(lua, 2);
    luaL_argcheck(lua, n > 0 && n < NSIG, 2, "invalid signal");
    return operation_result(lua, process_handle(lua).process.signal(static_cast<int>(n)));
}
int read_output(lua_State* lua) {
    static const char* streams[] = {"stdout", "stderr", nullptr};
    const bool error = check_option(lua, 2, nullptr, streams) == 1;
    const auto offset = check_integer(lua, 3), limit = check_integer(lua, 4);
    luaL_argcheck(lua, offset >= 0 && limit >= 0 && limit <= 65536, 3, "invalid output range");
    const auto bytes = process_handle(lua).process.output(error);
    luaL_argcheck(lua, static_cast<std::size_t>(offset) <= bytes.size(), 3, "offset exceeds stream size");
    const std::string chunk(bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(limit)));
    return protected_call(lua, [&](lua_State* lua) { lua_pushlstring(lua, chunk.data(), chunk.size()); return 1; }, 1);
}

int launch_application(lua_State* lua) {
    if (invocation(lua).closing) return luaL_error(lua, "invocation is shutting down");
    std::vector<std::string> arguments;
    std::string directory;
    lua_settop(lua, 2);
    lua_pushvalue(lua, 1);
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        parse_arguments(lua, 2, arguments);
        if (!lua_isnil(lua, 3)) {
            std::size_t length;
            const char* value = check_string(lua, 3, &length);
            luaL_argcheck(lua, length <= 16384, 3, "directory exceeds 16 KiB");
            directory.assign(value, length);
        }
        return 0;
    }, 0, 0, 2) < 0) return -1;
    const auto result = host::launch(arguments, directory);
    return operation_result(lua, result);
}

std::string_view file_path(lua_State* lua, int index) {
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    std::size_t n;
    const char* p = check_string(lua, index, &n);
    luaL_argcheck(lua, n <= 16384, index, "path exceeds 16 KiB");
    return {p, n};
}

int read_file(lua_State* lua) {
    const auto path = file_path(lua, 1);
    const auto limit = check_integer(lua, 2);
    luaL_argcheck(lua, limit >= 0 && limit <= 16 * 1024 * 1024, 2, "read limit must be 0..16 MiB");
    const auto result = host::read_file(std::string(path), static_cast<std::size_t>(limit));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushlstring(lua, result->data(), result->size()); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

template<bool directory>
int temporary(lua_State* lua) {
    const auto result = host::temporary(std::string(file_path(lua, 1)), directory);
    const int status = protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushstring(lua, result->c_str()); return 1; }
        lua_pushnil(lua); lua_pushstring(lua, result.error().c_str()); return 2;
    }, 2);
    if (status < 0 && result) { if (directory) rmdir(result->c_str()); else unlink(result->c_str()); }
    return status;
}
int make_directory(lua_State* lua) {
    return operation_result(lua, host::make_directory(std::string(file_path(lua, 1))));
}
template<bool replace>
int write_file(lua_State* lua) {
    const auto path = file_path(lua, 1);
    std::size_t n; const char* contents = check_string(lua, 2, &n);
    return operation_result(lua, host::write_file(std::string(path), {contents, n}, replace));
}
int copy_file(lua_State* lua) {
    const auto from = file_path(lua, 1), to = file_path(lua, 2);
    const auto limit = check_integer(lua, 3);
    luaL_argcheck(lua, limit >= 0, 3, "copy limit must be nonnegative");
    return operation_result(lua, host::copy_file(std::string(from), std::string(to), static_cast<std::size_t>(limit)));
}

constexpr char lock_type[] = "hyprworks.file_lock";
int close_lock(lua_State* lua) {
    static_cast<host::FileLock*>(luaL_checkudata(lua, 1, lock_type))->release();
    return 0;
}
int gc_lock(lua_State* lua) {
    auto* lock = static_cast<host::FileLock*>(luaL_checkudata(lua, 1, lock_type));
    lua_pushnil(lua); lua_setmetatable(lua, 1);
    std::destroy_at(lock); return 0;
}
int lock_file(lua_State* lua) {
    const auto path = file_path(lua, 1);
    auto* lock = new (lua_newuserdatauv(lua, sizeof(host::FileLock), 0)) host::FileLock();
    luaL_setmetatable(lua, lock_type);
    const auto result = lock->acquire(std::string(path));
    if (result) return 1;
    return protected_call(lua, [&](lua_State* lua) { lua_pushnil(lua); lua_pushstring(lua, result.error().c_str()); return 2; }, 2);
}

int update_file(lua_State* lua) {
    const auto name = file_path(lua, 1);
    const auto limit = check_integer(lua, 2);
    luaL_argcheck(lua, limit >= 0 && limit <= 16 * 1024 * 1024, 2, "file limit must be 0..16 MiB");
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    lua_pushvalue(lua, 3);
    const auto path = std::filesystem::absolute(name).string();
    host::FileLock lock;
    if (auto held = lock.acquire(path + ".lock"); !held) throw std::runtime_error(held.error());
    auto current = host::read_file(path, static_cast<std::size_t>(limit));
    if (!current) throw std::runtime_error(current.error());
    if (protected_call(lua, [&](lua_State* lua) {
        lua_pushlstring(lua, current->data(), current->size());
        lua_call(lua, 1, 1);
        if (!lua_isnil(lua, -1)) luaL_checktype(lua, -1, LUA_TSTRING);
        return 1;
    }, 1, 0, 1) < 0) return -1;
    std::size_t n = 0;
    const char* bytes = lua_tolstring(lua, -1, &n);
    if (n > static_cast<std::size_t>(limit)) throw std::runtime_error("edited file exceeds limit");
    const bool changed = bytes && std::string_view(bytes, n) != *current;
    if (changed) {
        host::FileWriter file(path);
        file.append({bytes, n}); file.commit(true);
    }
    lua_pushboolean(lua, changed); return 1;
}
int watch_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    std::size_t length;
    const char* path = check_string(lua, 1, &length);
    luaL_argcheck(lua, length <= 16384, 1, "path exceeds 16 KiB");
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool directory = lua_toboolean(lua, 3);
    auto& slot = vacant_slot(state.watches);
    auto watch = std::make_shared<WatchBinding>();
    const auto result = watch->watch.start(std::string(path, length), directory);
    if (!result) return operation_result(lua, result);
    return open_active(lua, slot, watch, watch_metatable, &watch->callback);
}

int watch_close(lua_State* lua) {
    auto& binding = watch_handle(lua);
    untrack(lua, invocation(lua).watches, binding); binding.watch.stop(); binding.pending = {}; ++binding.generation;
    unref(lua, binding.callback); return 0;
}

int watch_start(lua_State* lua) {
    auto& binding = watch_handle(lua);
    std::size_t length;
    const char* path = check_string(lua, 2, &length);
    luaL_argcheck(lua, length <= 16384, 2, "path exceeds 16 KiB");
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool closed = binding.closed;
    if (resume_handle(lua, invocation(lua).watches, binding) < 0) return -1;
    const auto result = binding.watch.start(std::string(path, length), lua_toboolean(lua, 3));
    if (!result && closed) untrack(lua, invocation(lua).watches, binding);
    binding.pending = {}; ++binding.generation;
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
        tzset();
        if (call_no_args(lua, state.on_clock_change) < 0) return -1;
    }
    for (auto timer : std::span(state.timers)) {
        if (!timer) continue;
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *timer;
        if (binding.closed || !binding.pending) continue;
        const auto ticks = std::exchange(binding.pending, 0);
        const auto generation = binding.generation;
        if (dispatch_callback(lua, binding, binding.callback, generation, [&](lua_State* lua) {
            lua_pushinteger(lua, static_cast<lua_Integer>(ticks));
            return 1;
        }) < 0) return -1;
    }
    for (auto process : std::span(state.processes)) {
        if (!process) continue;
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *process;
        const auto generation = binding.generation;
        for (unsigned i = 0; i < 2 && binding.on_output >= 0 && generation == binding.generation; ++i) {
            const auto bytes = binding.process.output(i != 0).substr(binding.delivered[i], 65536);
            if (bytes.empty()) continue;
            const std::string chunk(bytes);
            binding.delivered[i] += chunk.size();
            if (dispatch_callback(lua, binding, binding.on_output, generation, [&](lua_State* lua) {
                lua_pushstring(lua, i ? "stderr" : "stdout"); lua_pushlstring(lua, chunk.data(), chunk.size()); return 2;
            }) < 0) return -1;
        }
        if (generation != binding.generation) continue;
        if (std::exchange(binding.drained, false) &&
            dispatch_callback(lua, binding, binding.on_drain, generation, [&](lua_State* lua) {
                lua_pushinteger(lua, static_cast<lua_Integer>(binding.process.input_discarded())); return 1;
            }) < 0) return -1;
        if (generation != binding.generation || binding.output_pending() || !std::exchange(binding.pending, false) || binding.on_exit < 0) continue;
        const auto result = binding.process.result();
        if (dispatch_callback(lua, binding, binding.on_exit, generation, [&](lua_State* lua) {
            push_process_result(lua, result);
            return 1;
        }) < 0) return -1;
    }
    for (auto watch : std::span(state.watches)) {
        if (!watch) continue;
        if (state.requests.load(std::memory_order_relaxed)) return 0;
        auto& binding = *watch;
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
    luaL_newmetatable(lua, lock_type);
    const luaL_Reg locks[] = {{"close", close_lock}, {nullptr, nullptr}};
    userdata_methods(lua, locks);
    lua_pushcfunction(lua, close_lock); lua_setfield(lua, -2, "__close");
    lua_pushcfunction(lua, gc_lock); lua_setfield(lua, -2, "__gc");
    lua_pushliteral(lua, "hyprworks file lock"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    const luaL_Reg files[] = {{"temp_file", guarded<temporary<false>>}, {"temp_dir", guarded<temporary<true>>},
        {"mkdir", guarded<make_directory>}, {"create_file", guarded<write_file<false>>}, {"write_file", guarded<write_file<true>>},
        {"copy_file", guarded<copy_file>},
        {"lock_file", guarded<lock_file>}, {"update_file", operation_call<update_file>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, files, 0);
    lua_pushcfunction(lua, operation_call<on_clock_change>); lua_setfield(lua, -2, "on_clock_change");
    lua_pushcfunction(lua, guarded<timer_open>); lua_setfield(lua, -2, "timer");
    lua_pushcfunction(lua, guarded<process_open>); lua_setfield(lua, -2, "process");
    lua_pushcfunction(lua, guarded<launch_application>); lua_setfield(lua, -2, "launch");
    lua_pushcfunction(lua, guarded<read_file>); lua_setfield(lua, -2, "read_file");
    lua_pushcfunction(lua, guarded<watch_open>); lua_setfield(lua, -2, "watch_file");
    const luaL_Reg timer_methods[] = {{"start", guarded<timer_start>}, {"stop", guarded<timer_stop>},
        {"close", guarded<timer_close>}, {"on_tick", guarded<timer_callback>}, {nullptr, nullptr}};
    const luaL_Reg process_methods[] = {{"start", guarded<process_start>}, {"close", guarded<process_close>},
        {"running", guarded<process_running>}, {"result", guarded<process_result>}, {"on_exit", guarded<process_callback>},
        {"write", guarded<process_write>}, {"close_input", guarded<close_input>}, {"resize", guarded<process_resize>},
        {"signal", guarded<process_signal>}, {"hangup", guarded<process_hangup>}, {"on_output", guarded<on_output>}, {"on_drain", guarded<on_drain>},
        {"read_output", guarded<read_output>}, {nullptr, nullptr}};
    const luaL_Reg watch_methods[] = {{"close", guarded<watch_close>}, {"start", guarded<watch_start>}, {"on_change", guarded<watch_callback>}, {nullptr, nullptr}};
    const struct { const char* name; const luaL_Reg* methods; lua_CFunction close, gc; } metatables[] = {
        {timer_metatable, timer_methods, guarded<timer_close>, release_userdata<TimerBinding>},
        {process_metatable, process_methods, guarded<process_close>, release_userdata<ProcessBinding>},
        {watch_metatable, watch_methods, guarded<watch_close>, release_userdata<WatchBinding>}
    };
    for (const auto& metatable : metatables) {
        luaL_newmetatable(lua, metatable.name);
        lua_pushcfunction(lua, metatable.gc); lua_setfield(lua, -2, "__gc");
        userdata_methods(lua, metatable.methods);
        lua_pushcfunction(lua, metatable.close); lua_setfield(lua, -2, "__close");
        lua_pushliteral(lua, "hyprworks host utility"); lua_setfield(lua, -2, "__metatable");
        lua_pop(lua, 1);
    }
}

}
