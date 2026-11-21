#include "../runtime/invocation.hpp"

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char command_metatable[] = "hyprworks.commands";

CommandBinding& command_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<CommandBinding**>(luaL_checkudata(lua, 1, command_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "command server is closed");
    return binding;
}

int command_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.commands) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        std::string name;
        std::vector<std::string> commands;
        lua_pushvalue(lua, 1);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_getfield(lua, 2, "name"); std::size_t length; const char* value = luaL_checklstring(lua, -1, &length);
            luaL_argcheck(lua, length <= 255, 2, "command bus name exceeds 255 bytes"); name.assign(value, length); lua_pop(lua, 1);
            lua_getfield(lua, 2, "commands"); luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1); luaL_argcheck(lua, count > 0 && count <= 128, 2, "expected 1..128 command names");
            for (std::size_t i = 0; i < count; ++i) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); value = luaL_checklstring(lua, -1, &length);
                luaL_argcheck(lua, length <= 128, 2, "command name exceeds 128 bytes"); commands.emplace_back(value, length); lua_pop(lua, 1);
            }
            return 0;
        }, 0, 0, 1) < 0) return -1;
        if (state.commands) throw std::runtime_error("command server opened while parsing options");
        auto& binding = state.commands.emplace();
        binding.name = std::move(name);
        binding.commands = std::move(commands);
        const auto result = binding.server.start(binding.name, binding.commands);
        if (!result) {
            state.commands.reset();
            return operation_result(lua, result);
        }
    } else if (!lua_isnoneornil(lua, 1)) return luaL_error(lua, "command server already exists; use hw.commands() to share it");
    auto& binding = *state.commands;
    const int result = retain_handle(lua, binding, command_metatable);
    if (result < 0) { binding.closed = true; binding.server.stop(); }
    return result;
}

int command_status(lua_State* lua) {
    const auto& binding = command_handle(lua, true); const auto status = binding.server.status(); const bool closed = binding.closed;
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "ready", "failed"}; lua_createtable(lua, 0, 2);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int command_on_status(lua_State* lua) {
    auto& binding = command_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int command_on_request(lua_State* lua) {
    auto& binding = command_handle(lua);
    replace_callback(lua, 2, binding.on_request, binding.closed);
    return 0;
}

int command_restart(lua_State* lua) {
    auto& binding = command_handle(lua, true); const auto result = binding.server.start(binding.name, binding.commands);
    binding.closed = false; ++binding.generation; binding.status_pending = true; binding.requests.clear();
    return operation_result(lua, result);
}

int command_close(lua_State* lua) {
    auto& binding = command_handle(lua, true); binding.server.stop(); binding.closed = true; ++binding.generation;
    binding.requests.clear(); binding.status_pending = false;
    unref(lua, binding.on_status);
    unref(lua, binding.on_request); return 0;
}

}

int dispatch_commands(lua_State* lua, CommandBinding& binding) {
    const auto requests = std::exchange(binding.requests, {}); const bool status = std::exchange(binding.status_pending, false);
    const auto generation = binding.generation;
    for (std::size_t i = 0; i <= requests.size(); ++i) {
        if (binding.closed || binding.generation != generation || invocation(lua).quit_requested.load(std::memory_order_relaxed)) break;
        if (!i && !status) continue;
        const int reference = i ? binding.on_request : binding.on_status;
        bool success = true, delivered = false;
        std::string response;
        if (reference >= 0) {
            lua_pushcfunction(lua, traceback);
            if (protected_call(lua, [&](lua_State* lua) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, reference); lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
                if (i) {
                    const auto& request = requests[i - 1]; lua_createtable(lua, 0, 4);
                    identifier(lua, request.id); lua_setfield(lua, -2, "id"); string_field(lua, "name", request.name); string_field(lua, "sender", request.sender);
                    push_strings(lua, request.arguments);
                    lua_setfield(lua, -2, "arguments");
                }
                if (binding.closed || binding.generation != generation || reference != (i ? binding.on_request : binding.on_status)) return 0;
                delivered = true; lua_call(lua, i ? 2 : 1, i ? 2 : 0);
                if (i) {
                    success = !lua_isnil(lua, -2) || lua_isnil(lua, -1);
                    const int result = success ? -2 : -1;
                    if (!lua_isnil(lua, result)) {
                        luaL_checktype(lua, result, LUA_TSTRING); std::size_t size; const char* text = lua_tolstring(lua, result, &size);
                        luaL_argcheck(lua, size <= 65536, result, "command response exceeds 64 KiB"); response.assign(text, size);
                    }
                }
                return 0;
            }, 0, 1) < 0) return -1;
            lua_settop(lua, 0);
        }
        if (i && !binding.closed && binding.generation == generation) {
            if (!delivered) { success = false; response = "command callback is not installed"; }
            const auto result = binding.server.respond(requests[i - 1].id, response, success);
            if (!result) throw std::runtime_error(result.error());
        }
    }
    return 0;
}

void register_commands(lua_State* lua) {
    luaL_newmetatable(lua, command_metatable);
    const luaL_Reg command_methods[] = {{"status", guarded<command_status>}, {"on_status", guarded<command_on_status>},
        {"on_request", guarded<command_on_request>}, {"restart", guarded<command_restart>}, {"close", guarded<command_close>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, command_methods, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<command_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks command server"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<command_open>); lua_setfield(lua, -2, "commands");
}

}
