#include "../invocation.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char workspace_metatable[] = "hyprworks.workspaces";
constexpr char error_metatable[] = "hyprworks.error";
constexpr std::size_t max_workspace_handles = 32;
constexpr std::size_t max_listeners = 128;
using Listener = WorkspaceBinding::Listener;

WorkspaceBinding& workspace(lua_State* lua, int index = 1, bool allow_closed = false) {
    auto& binding = **static_cast<WorkspaceBinding**>(luaL_checkudata(lua, index, workspace_metatable));
    if (invocation(lua).closing)
        luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed)
        luaL_error(lua, "workspace connection is closed");
    return binding;
}

void push_error(lua_State* lua, const native::Error& error) {
    static const char* codes[] = {"invalid_target", "unavailable", "io", "timeout", "protocol",
        "limit_exceeded", "not_ready", "unknown_workspace", "busy"};
    lua_createtable(lua, 0, 4);
    lua_pushstring(lua, codes[static_cast<unsigned>(error.code)]);
    lua_setfield(lua, -2, "code");
    string_field(lua, "operation", error.operation);
    string_field(lua, "message", error.message);
    if (error.system_error) {
        lua_pushinteger(lua, error.system_error);
        lua_setfield(lua, -2, "system_error");
    }
    luaL_setmetatable(lua, error_metatable);
}

int error_tostring(lua_State* lua) {
    lua_getfield(lua, 1, "operation");
    lua_getfield(lua, 1, "message");
    lua_pushfstring(lua, "%s: %s", luaL_checkstring(lua, -2), luaL_checkstring(lua, -1));
    return 1;
}

void push_key(lua_State* lua, const native::WorkspaceKey& key) {
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, key.type == native::WorkspaceType::normal ? "normal" : "special");
    lua_setfield(lua, -2, "type");
    string_field(lua, "address", key.address);
}

int push_snapshot(lua_State* lua, const std::optional<native::Snapshot>& snapshot) {
    if (!snapshot) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, 0, 4);
    identifier(lua, snapshot->revision);
    lua_setfield(lua, -2, "revision");
    lua_createtable(lua, static_cast<int>(snapshot->workspaces.size()), 0);
    lua_Integer index = 1;
    for (const auto& record : snapshot->workspaces) {
        push_key(lua, record.key);
        string_field(lua, "name", record.name);
        if (record.number) {
            lua_pushinteger(lua, *record.number);
            lua_setfield(lua, -2, "number");
        }
        if (record.output_id) {
            lua_pushinteger(lua, *record.output_id);
            lua_setfield(lua, -2, "output_id");
        }
        lua_rawseti(lua, -2, index++);
    }
    lua_setfield(lua, -2, "workspaces");
    lua_createtable(lua, static_cast<int>(snapshot->outputs.size()), 0);
    index = 1;
    for (const auto& output : snapshot->outputs) {
        lua_createtable(lua, 0, 5);
        lua_pushinteger(lua, output.id);
        lua_setfield(lua, -2, "id");
        string_field(lua, "name", output.name);
        lua_pushboolean(lua, output.enabled);
        lua_setfield(lua, -2, "enabled");
        if (output.active_workspace) {
            push_key(lua, *output.active_workspace);
            lua_setfield(lua, -2, "active_workspace");
        }
        if (output.special_workspace) {
            push_key(lua, *output.special_workspace);
            lua_setfield(lua, -2, "special_workspace");
        }
        lua_rawseti(lua, -2, index++);
    }
    lua_setfield(lua, -2, "outputs");
    if (snapshot->focused_output) {
        lua_pushinteger(lua, *snapshot->focused_output);
        lua_setfield(lua, -2, "focused_output");
    }
    return 1;
}

int push_status(lua_State* lua, const native::ConnectionStatus& status, bool closed, bool has_snapshot) {
    static const char* phases[] = {"stopped", "connecting", "synchronizing", "ready", "disconnected", "failed"};
    lua_createtable(lua, 0, 4);
    lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]);
    lua_setfield(lua, -2, "phase");
    lua_pushboolean(lua, status.refresh_pending);
    lua_setfield(lua, -2, "refresh_pending");
    lua_pushboolean(lua, has_snapshot);
    lua_setfield(lua, -2, "has_snapshot");
    if (status.error) {
        push_error(lua, *status.error);
        lua_setfield(lua, -2, "error");
    }
    return 1;
}

void push_switch_result(lua_State* lua, const native::SwitchResult& result) {
    static const char* outcomes[] = {"acknowledged", "compositor_error", "failed", "cancelled"};
    lua_createtable(lua, 0, 4);
    identifier(lua, result.id);
    lua_setfield(lua, -2, "id");
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result.outcome)]);
    lua_setfield(lua, -2, "outcome");
    lua_pushboolean(lua, result.may_have_executed);
    lua_setfield(lua, -2, "may_have_executed");
    if (result.error) {
        push_error(lua, *result.error);
        lua_setfield(lua, -2, "error");
    }
}

int push_operation(lua_State* lua, const std::expected<void, native::Error>& result) {
    if (result) {
        lua_pushboolean(lua, true);
        return 1;
    }
    lua_pushnil(lua);
    push_error(lua, result.error());
    return 2;
}

int push_request(lua_State* lua, const std::expected<std::uint64_t, native::Error>& result) {
    if (result) {
        identifier(lua, *result);
        return 1;
    }
    lua_pushnil(lua);
    push_error(lua, result.error());
    return 2;
}

std::expected<WorkspaceBinding*, native::Error> open_workspace(InvocationState& state, const char* path, std::size_t length) {
    auto target = path ? std::expected<native::SessionTarget, native::Error>(native::SessionTarget{std::string(path, length)}) :
                         native::session_from_environment();
    if (!target)
        return std::unexpected(target.error());
    if (target->directory.empty() || target->directory.front() != '/' || target->directory.contains('\0'))
        return std::unexpected(native::Error{native::ErrorCode::invalid_target, "session", "session directory must be an absolute path without NUL"});
    target->directory = std::filesystem::path(target->directory).lexically_normal().string();
    if (target->directory.size() > 1 && target->directory.back() == '/')
        target->directory.pop_back();
    for (const auto& existing : state.workspaces) {
        if (!existing->closed && existing->target.directory == target->directory) {
            if (existing->handle < 0)
                return std::unexpected(native::Error{native::ErrorCode::busy, "workspaces", "connection is being opened"});
            return existing.get();
        }
    }
    if (state.workspaces.size() == max_workspace_handles)
        return std::unexpected(native::Error{native::ErrorCode::limit_exceeded, "workspaces", "invocation workspace handle limit reached"});
    auto binding = std::make_unique<WorkspaceBinding>();
    binding->target = std::move(*target);
    if (auto started = binding->connection.start(binding->target); !started)
        return std::unexpected(started.error());
    auto* pointer = binding.get();
    state.workspaces.push_back(std::move(binding));
    return pointer;
}

int push_workspace(lua_State* lua, const std::expected<WorkspaceBinding*, native::Error>& result) {
    if (!result) {
        lua_pushnil(lua);
        push_error(lua, result.error());
        return 2;
    }
    auto& binding = **result;
    return retain_handle(lua, binding, workspace_metatable);
}

int workspace_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing)
        return luaL_error(lua, "invocation is shutting down");
    std::size_t length = 0;
    const char* path = lua_isnoneornil(lua, 1) ? nullptr : luaL_checklstring(lua, 1, &length);
    auto result = open_workspace(state, path, length);
    const int status = protected_call(lua, [&](lua_State* lua) { return push_workspace(lua, result); }, 2);
    if (status < 0 && result && (*result)->handle == LUA_NOREF) {
        (*result)->closed = true;
        (*result)->connection.stop();
    }
    return status;
}

int workspace_snapshot(lua_State* lua) {
    auto& binding = workspace(lua);
    // A Lua allocation can run finalizers that restart or close the native connection.
    auto snapshot = binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) { return push_snapshot(lua, snapshot); }, 1);
}

int workspace_status(lua_State* lua) {
    auto& binding = workspace(lua, 1, true);
    const auto status = binding.connection.status();
    return protected_call(lua, [&, closed = binding.closed, has_snapshot = binding.connection.snapshot().has_value()](lua_State* lua) {
        return push_status(lua, status, closed, has_snapshot);
    }, 1);
}

int workspace_switch(lua_State* lua) {
    auto& binding = workspace(lua);
    static const char* types[] = {"normal", "special", nullptr};
    const int type = luaL_checkoption(lua, 2, nullptr, types);
    std::size_t length;
    const char* address = luaL_checklstring(lua, 3, &length);
    auto result = binding.connection.switch_workspace({type == 0 ? native::WorkspaceType::normal : native::WorkspaceType::special,
                                                       std::string(address, length)});
    return protected_call(lua, [&](lua_State* lua) { return push_request(lua, result); }, 2);
}

int workspace_refresh(lua_State* lua) {
    auto& binding = workspace(lua);
    const auto before = binding.connection.status();
    auto result = binding.connection.refresh();
    binding.status_changed |= before != binding.connection.status();
    return protected_call(lua, [&](lua_State* lua) { return push_operation(lua, result); }, 2);
}

int workspace_restart(lua_State* lua) {
    auto& binding = workspace(lua);
    const auto before = binding.connection.status();
    const bool had_snapshot = binding.connection.snapshot().has_value();
    auto result = binding.connection.start(binding.target);
    if (result || before != binding.connection.status()) ++binding.generation;
    binding.status_changed |= before != binding.connection.status();
    binding.snapshot_changed |= had_snapshot && !binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) { return push_operation(lua, result); }, 2);
}

void remove_listener(lua_State* lua, WorkspaceBinding& binding, lua_Integer id) {
    auto found = std::ranges::find(binding.listeners, id, &Listener::id);
    if (found != binding.listeners.end()) {
        const int reference = found->reference;
        binding.listeners.erase(found);
        luaL_unref(lua, LUA_REGISTRYINDEX, reference);
    }
}

int unsubscribe(lua_State* lua) {
    auto& binding = workspace(lua, lua_upvalueindex(1), true);
    remove_listener(lua, binding, lua_tointeger(lua, lua_upvalueindex(2)));
    return 0;
}

template<Event Kind>
int subscribe(lua_State* lua) {
    auto& binding = workspace(lua);
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    if (binding.listeners.size() == max_listeners || binding.last_listener_id == LUA_MAXINTEGER)
        return luaL_error(lua, "workspace subscription limit reached");
    const auto id = ++binding.last_listener_id;
    binding.listeners.push_back(Listener{id, Kind, LUA_NOREF, Kind != Event::result});
    lua_pushvalue(lua, 1);
    lua_pushvalue(lua, 2);
    const int status = protected_call(lua, [&](lua_State* lua) {
        lua_pushvalue(lua, 3);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        auto found = std::ranges::find(binding.listeners, id, &Listener::id);
        if (binding.closed || found == binding.listeners.end()) {
            luaL_unref(lua, LUA_REGISTRYINDEX, reference);
            return luaL_error(lua, "connection closed during subscription");
        }
        found->reference = reference;
        lua_pushvalue(lua, 2);
        lua_pushinteger(lua, id);
        lua_pushcclosure(lua, guarded<unsubscribe>, 2);
        return 1;
    }, 1, 0, 2);
    if (status < 0)
        remove_listener(lua, binding, id);
    return status;
}

int workspace_close(lua_State* lua) {
    auto& binding = workspace(lua, 1, true);
    binding.closed = true;
    ++binding.generation;
    binding.result.reset();
    binding.status_changed = false;
    binding.snapshot_changed = false;
    auto cancelled = binding.connection.stop();
    return protected_call(lua, [&](lua_State* lua) {
        for (const auto& listener : binding.listeners)
            luaL_unref(lua, LUA_REGISTRYINDEX, listener.reference);
        binding.listeners.clear();
        if (cancelled)
            push_switch_result(lua, *cancelled);
        else
            lua_pushnil(lua);
        return 1;
    }, 1);
}

struct Notification {
    WorkspaceBinding* binding;
    std::uint64_t generation;
    std::optional<native::SwitchResult> result;
    std::array<lua_Integer, max_listeners> listeners{};
    std::size_t count = 0;
};

}

bool active(const WorkspaceBinding& binding) {
    const auto phase = binding.connection.status().phase;
    return !binding.closed && (phase == native::ConnectionPhase::connecting ||
                              phase == native::ConnectionPhase::synchronizing || phase == native::ConnectionPhase::ready);
}

bool pending(const WorkspaceBinding& binding) {
    return !binding.closed && (binding.status_changed || binding.snapshot_changed || binding.result ||
                              std::ranges::any_of(binding.listeners, &Listener::initial));
}

void register_workspaces(lua_State* lua) {
    luaL_newmetatable(lua, error_metatable);
    lua_pushcfunction(lua, error_tostring);
    lua_setfield(lua, -2, "__tostring");
    lua_pop(lua, 1);
    luaL_newmetatable(lua, workspace_metatable);
    const luaL_Reg methods[] = {
        {"snapshot", guarded<workspace_snapshot>}, {"status", guarded<workspace_status>},
        {"switch", guarded<workspace_switch>}, {"refresh", guarded<workspace_refresh>},
        {"restart", guarded<workspace_restart>}, {"close", guarded<workspace_close>},
        {"on_change", guarded<subscribe<Event::change>>}, {"on_status", guarded<subscribe<Event::status>>},
        {"on_result", guarded<subscribe<Event::result>>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, methods, 0);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<workspace_close>);
    lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks workspace connection");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<workspace_open>);
    lua_setfield(lua, -2, "workspaces");
}

int dispatch_workspaces(lua_State* lua, InvocationState& state) {
    std::vector<Notification> notifications;
    for (const auto& binding : state.workspaces) {
        if (!pending(*binding))
            continue;
        const bool status = std::exchange(binding->status_changed, false);
        const bool change = std::exchange(binding->snapshot_changed, false);
        auto& notification = notifications.emplace_back(binding.get(), binding->generation, std::exchange(binding->result, {}));
        for (const auto event : {Event::status, Event::change, Event::result}) {
            const bool changed = event == Event::status ? status : event == Event::change ? change : notification.result.has_value();
            for (auto& listener : binding->listeners) {
                if (listener.event == event && (listener.initial || changed)) {
                    listener.initial = false;
                    notification.listeners[notification.count++] = listener.id;
                }
            }
        }
    }
    for (const auto& notification : notifications) {
        auto& binding = *notification.binding;
        for (std::size_t i = 0; i < notification.count; ++i) {
            if (state.quit_requested.load(std::memory_order_relaxed))
                return 0;
            if (binding.closed || binding.generation != notification.generation)
                break;
            const auto found = std::ranges::find(binding.listeners, notification.listeners[i], &Listener::id);
            if (found == binding.listeners.end() || found->reference == LUA_NOREF)
                continue;
            const auto id = found->id;
            const int reference = found->reference;
            const auto* result = found->event == Event::result ? &*notification.result : nullptr;
            lua_pushcfunction(lua, traceback);
            const int status = protected_call(lua, [&](lua_State* lua) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
                lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
                if (result)
                    push_switch_result(lua, *result);
                // Marshalling may run finalizers that close, restart or unsubscribe.
                if (binding.closed || binding.generation != notification.generation ||
                    std::ranges::find(binding.listeners, id, &Listener::id) == binding.listeners.end())
                    return 0;
                lua_call(lua, result ? 2 : 1, 0);
                return 0;
            }, 0, 1);
            if (status < 0)
                return -1;
            lua_settop(lua, 0);
        }
    }
    return 0;
}

}
