#include "../runtime/invocation.hpp"

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char desktop_metatable[] = "hyprworks.desktop_entries";

DesktopBinding& desktop_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<DesktopBinding**>(luaL_checkudata(lua, 1, desktop_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "desktop entries are closed");
    return binding;
}

int desktop_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.desktop) {
        auto& binding = state.desktop.emplace();
        const auto result = binding.entries.start();
        if (!result) {
            state.desktop.reset();
            return operation_result(lua, result);
        }
    }
    auto& binding = *state.desktop;
    const int result = retain_handle(lua, binding, desktop_metatable);
    if (result < 0) { binding.closed = true; binding.entries.stop(); }
    return result;
}

int desktop_status(lua_State* lua) {
    const auto& binding = desktop_handle(lua, true);
    const auto status = binding.entries.status(); const bool closed = binding.closed, snapshot = bool(binding.entries.snapshot());
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "ready", "failed"}; lua_createtable(lua, 0, 3);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        lua_pushboolean(lua, snapshot); lua_setfield(lua, -2, "has_snapshot");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int desktop_snapshot(lua_State* lua) {
    const auto snapshot = desktop_handle(lua, true).entries.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 2); identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        lua_createtable(lua, static_cast<int>(snapshot->entries.size()), 0); lua_Integer index = 0;
        for (const auto& entry : snapshot->entries) {
            lua_createtable(lua, 0, 15);
            for (const auto& [key, text] : {std::pair{"id", &entry.id}, {"name", &entry.name}, {"generic_name", &entry.generic_name},
                {"comment", &entry.comment}, {"icon", &entry.icon}, {"executable", &entry.executable}, {"filename", &entry.filename}}) string_field(lua, key, *text);
            for (const auto& [key, flag] : {std::pair{"visible", entry.visible}, {"terminal", entry.terminal},
                {"supports_files", entry.supports_files}, {"supports_uris", entry.supports_uris}}) { lua_pushboolean(lua, flag); lua_setfield(lua, -2, key); }
            for (const auto& [key, list] : {std::pair{"keywords", &entry.keywords}, {"categories", &entry.categories}, {"mime_types", &entry.mime_types}}) {
                push_strings(lua, *list);
                lua_setfield(lua, -2, key);
            }
            lua_createtable(lua, static_cast<int>(entry.actions.size()), 0); lua_Integer item = 0;
            for (const auto& action : entry.actions) {
                lua_createtable(lua, 0, 2); string_field(lua, "id", action.id); string_field(lua, "name", action.name); lua_rawseti(lua, -2, ++item);
            }
            lua_setfield(lua, -2, "actions"); lua_rawseti(lua, -2, ++index);
        }
        lua_setfield(lua, -2, "entries"); return 1;
    }, 1);
}

int desktop_on_status(lua_State* lua) {
    auto& binding = desktop_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int desktop_on_change(lua_State* lua) {
    auto& binding = desktop_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int desktop_refresh(lua_State* lua) {
    desktop_handle(lua).entries.refresh();
    return 0;
}

int desktop_restart(lua_State* lua) {
    auto& binding = desktop_handle(lua, true);
    const auto result = binding.entries.start(); binding.closed = false; ++binding.generation;
    binding.status_pending = true; binding.change_pending = result.has_value();
    return operation_result(lua, result);
}

int desktop_close(lua_State* lua) {
    auto& binding = desktop_handle(lua, true);
    binding.entries.stop(); binding.closed = true; ++binding.generation; binding.status_pending = binding.change_pending = false;
    unref(lua, binding.on_status);
    unref(lua, binding.on_change); return 0;
}

int desktop_launch(lua_State* lua) {
    auto& binding = desktop_handle(lua);
    std::string id, action; std::vector<std::string> uris;
    lua_settop(lua, 4); lua_pushvalue(lua, 2); lua_pushvalue(lua, 3); lua_pushvalue(lua, 4);
    if (protected_call(lua, [&](lua_State* lua) {
        std::size_t length; const char* text = luaL_checklstring(lua, 2, &length);
        luaL_argcheck(lua, length <= 65536, 2, "desktop id exceeds 64 KiB"); id.assign(text, length);
        if (!lua_isnil(lua, 3)) {
            luaL_checktype(lua, 3, LUA_TTABLE); const auto count = lua_rawlen(lua, 3);
            luaL_argcheck(lua, count <= 256, 3, "expected at most 256 URIs"); std::size_t bytes = 0;
            for (std::size_t i = 0; i < count; ++i) {
                lua_rawgeti(lua, 3, static_cast<lua_Integer>(i + 1)); text = luaL_checklstring(lua, -1, &length);
                luaL_argcheck(lua, length <= 65536 && (bytes += length) <= 1024 * 1024, 3, "URI budget exceeded");
                uris.emplace_back(text, length); lua_pop(lua, 1);
            }
        }
        if (!lua_isnil(lua, 4)) {
            text = luaL_checklstring(lua, 4, &length); luaL_argcheck(lua, length <= 65536, 4, "action id exceeds 64 KiB"); action.assign(text, length);
        }
        return 0;
    }, 0, 0, 3) < 0) return -1;
    if (binding.closed) throw std::runtime_error("desktop entries closed while parsing launch");
    const auto result = binding.entries.launch(id, uris, action);
    return operation_result(lua, result);
}

}

int dispatch_desktop(lua_State* lua, DesktopBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false), change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    for (bool is_status : {true, false}) {
        if (!(is_status ? status : change)) continue;
        const int& callback = is_status ? binding.on_status : binding.on_change;
        if (dispatch_callback(lua, binding, callback, generation, [](lua_State*) { return 0; }) < 0) return -1;
    }
    return 0;
}

void register_desktop(lua_State* lua) {
    luaL_newmetatable(lua, desktop_metatable);
    const luaL_Reg desktop_methods[] = {{"status", guarded<desktop_status>}, {"snapshot", guarded<desktop_snapshot>},
        {"on_status", guarded<desktop_on_status>}, {"on_change", guarded<desktop_on_change>},
        {"refresh", guarded<desktop_refresh>}, {"restart", guarded<desktop_restart>},
        {"launch", guarded<desktop_launch>}, {"close", guarded<desktop_close>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, desktop_methods, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<desktop_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks desktop entries"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<desktop_open>); lua_setfield(lua, -2, "desktop_entries");
}

}
