#include "../../runtime/invocation.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char workspace_metatable[] = "hyprworks.workspaces";
constexpr char error_metatable[] = "hyprworks.error";

WorkspaceBinding& workspace(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<std::shared_ptr<WorkspaceBinding>*>(luaL_checkudata(lua, 1, workspace_metatable));
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
    lua_pushfstring(lua, "%s: %s", check_string(lua, -2), check_string(lua, -1));
    return 1;
}

void push_key(lua_State* lua, const native::WorkspaceKey& key) {
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, key.type == native::WorkspaceType::normal ? "normal" : "special");
    lua_setfield(lua, -2, "type");
    string_field(lua, "address", key.address);
}

int push_snapshot(lua_State* lua, const std::shared_ptr<const native::Snapshot>& snapshot) {
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

int workspace_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    std::size_t length = 0;
    const char* path = lua_isnoneornil(lua, 1) ? nullptr : check_string(lua, 1, &length);
    const auto failure = [&](const native::Error& error) {
        return protected_call(lua, [&](lua_State* lua) { lua_pushnil(lua); push_error(lua, error); return 2; }, 2);
    };
    auto target = path ? std::expected<native::SessionTarget, native::Error>(native::SessionTarget{std::string(path, length)}) :
                         native::session_from_environment();
    if (!target)
        return failure(target.error());
    target->directory = std::filesystem::path(target->directory).lexically_normal().string();
    if (target->directory.size() > 1 && target->directory.back() == '/')
        target->directory.pop_back();
    for (const auto& existing : state.workspaces) {
        if (existing && existing->target.directory == target->directory) {
            if (existing->handle < 0)
                return failure({native::ErrorCode::busy, "workspaces", "connection is being opened"});
            lua_rawgeti(lua, LUA_REGISTRYINDEX, existing->handle);
            return 1;
        }
    }
    auto& slot = vacant_slot(state.workspaces);
    auto binding = std::make_shared<WorkspaceBinding>();
    binding->target = std::move(*target);
    if (auto started = binding->connection.start(binding->target); !started)
        return failure(started.error());
    return open_active(lua, slot, binding, workspace_metatable);
}

int workspace_snapshot(lua_State* lua) {
    auto& binding = workspace(lua);
    auto snapshot = binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) { return push_snapshot(lua, snapshot); }, 1);
}

int workspace_status(lua_State* lua) {
    auto& binding = workspace(lua, true);
    const auto status = binding.connection.status();
    return protected_call(lua, [&, closed = binding.closed, has_snapshot = bool(binding.connection.snapshot())](lua_State* lua) {
        return push_status(lua, status, closed, has_snapshot);
    }, 1);
}

int workspace_switch(lua_State* lua) {
    static const char* types[] = {"normal", "special", nullptr};
    const int type = check_option(lua, 2, nullptr, types);
    std::size_t length;
    const char* address = check_string(lua, 3, &length);
    auto& binding = workspace(lua);
    auto result = binding.result ?
        std::expected<std::uint64_t, native::Error>(std::unexpected(native::Error{
            native::ErrorCode::busy, "switch", "a workspace result awaits delivery"})) :
        binding.connection.switch_workspace({type == 0 ? native::WorkspaceType::normal : native::WorkspaceType::special,
                                                        std::string(address, length)});
    return protected_call(lua, [&](lua_State* lua) { return push_request(lua, result); }, 2);
}

int workspace_refresh(lua_State* lua) {
    auto& binding = workspace(lua);
    const auto before = binding.connection.status();
    auto result = binding.connection.refresh();
    binding.status_pending |= before != binding.connection.status();
    return protected_call(lua, [&](lua_State* lua) { return push_operation(lua, result); }, 2);
}

int workspace_restart(lua_State* lua) {
    auto& binding = workspace(lua, true);
    if (resume_handle(lua, invocation(lua).workspaces, binding) < 0) return -1;
    const auto before = binding.connection.status();
    const bool had_snapshot = bool(binding.connection.snapshot());
    auto result = binding.result ?
        std::expected<void, native::Error>(std::unexpected(native::Error{
            native::ErrorCode::busy, "restart", "a workspace result awaits delivery"})) :
        binding.connection.start(binding.target);
    if (result || before != binding.connection.status()) ++binding.generation;
    binding.status_pending |= before != binding.connection.status();
    binding.change_pending |= had_snapshot && !binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) { return push_operation(lua, result); }, 2);
}

int on_status(lua_State* lua) {
    auto& binding = workspace(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int on_change(lua_State* lua) {
    auto& binding = workspace(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int on_result(lua_State* lua) {
    auto& binding = workspace(lua);
    replace_callback(lua, 2, binding.on_result, binding.closed);
    return 0;
}

int workspace_close(lua_State* lua) {
    auto& binding = workspace(lua, true);
    untrack(lua, invocation(lua).workspaces, binding);
    return close_action_observer(lua, binding, push_switch_result);
}

}

void register_workspaces(lua_State* lua) {
    luaL_newmetatable(lua, error_metatable);
    lua_pushcfunction(lua, error_tostring);
    lua_setfield(lua, -2, "__tostring");
    lua_pop(lua, 1);
    luaL_newmetatable(lua, workspace_metatable);
    lua_pushcfunction(lua, release_userdata<WorkspaceBinding>); lua_setfield(lua, -2, "__gc");
    const luaL_Reg methods[] = {
        {"snapshot", guarded<workspace_snapshot>}, {"status", guarded<workspace_status>},
        {"switch", guarded<workspace_switch>}, {"refresh", guarded<workspace_refresh>},
        {"restart", guarded<workspace_restart>}, {"close", guarded<workspace_close>},
        {"on_change", guarded<on_change>}, {"on_status", guarded<on_status>},
        {"on_result", guarded<on_result>}, {nullptr, nullptr}
    };
    userdata_methods(lua, methods);
    lua_pushcfunction(lua, guarded<workspace_close>);
    lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks workspace connection");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<workspace_open>);
    lua_setfield(lua, -2, "workspaces");
}

int dispatch_workspaces(lua_State* lua, InvocationState& state) {
    for (auto binding : std::span(state.workspaces))
        if (binding && deliver_observer(lua, *binding, push_switch_result) < 0) return -1;
    return 0;
}

}
