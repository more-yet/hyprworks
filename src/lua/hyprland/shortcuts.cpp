#include "../../runtime/invocation.hpp"
#ifdef HYPRWORKS_HAS_GUI
#include "../../gui/gui.hpp"
#endif

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char shortcut_metatable[] = "hyprworks.hyprland.shortcuts";

std::expected<void, std::string> start_shortcuts(InvocationState& state) {
    auto& binding = *state.shortcuts;
#ifdef HYPRWORKS_HAS_GUI
    if (state.gui) {
        if (!binding.display.empty())
            return std::unexpected("GUI shortcuts use the GUI display");
        return binding.connection.attach(state.gui->display, binding.app_id, binding.shortcuts, [&state]() noexcept { wake(state); });
    }
#endif
    return binding.connection.start(binding.app_id, binding.shortcuts, binding.display);
}

ShortcutBinding& shortcut_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<ShortcutBinding**>(luaL_checkudata(lua, 1, shortcut_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "shortcut connection is closed");
    return binding;
}

int shortcut_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.shortcuts) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        std::string app_id, display;
        std::vector<shortcuts::Shortcut> shortcuts;
        lua_pushvalue(lua, 1);
        if (protected_call(lua, [&](lua_State* lua) {
            for (const auto& [key, target] : {std::pair{"app_id", &app_id}, {"display", &display}}) {
                field(lua, 2, key);
                if (!lua_isnil(lua, -1)) {
                    std::size_t length; const char* value = check_string(lua, -1, &length);
                    luaL_argcheck(lua, length <= 4096, 2, "shortcut option exceeds 4096 bytes"); target->assign(value, length);
                }
                lua_pop(lua, 1);
            }
            field(lua, 2, "shortcuts"); luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1);
            luaL_argcheck(lua, count > 0 && count <= 128, 2, "expected 1..128 shortcuts");
            shortcuts.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); luaL_checktype(lua, -1, LUA_TTABLE);
                auto& shortcut = shortcuts[i];
                for (const auto& [key, target] : {std::pair{"id", &shortcut.id}, {"description", &shortcut.description}, {"trigger_description", &shortcut.trigger_description}}) {
                    field(lua, -1, key);
                    if (!lua_isnil(lua, -1)) {
                        std::size_t length; const char* value = check_string(lua, -1, &length);
                        luaL_argcheck(lua, length <= 4096, 2, "shortcut text exceeds 4096 bytes"); target->assign(value, length);
                    }
                    lua_pop(lua, 1);
                }
                lua_pop(lua, 1);
            }
            return 0;
        }, 0, 0, 1) < 0) return -1;
        if (state.shortcuts) throw std::runtime_error("shortcut connection opened while parsing options");
        auto& binding = state.shortcuts.emplace();
        binding.app_id = std::move(app_id);
        binding.display = std::move(display);
        binding.shortcuts = std::move(shortcuts);
        const auto result = start_shortcuts(state);
        if (!result) {
            state.shortcuts.reset();
            return operation_result(lua, result);
        }
    } else if (!lua_isnoneornil(lua, 1)) return luaL_error(lua, "shortcut connection already exists; use hw.hyprland.shortcuts() to share it");
    auto& binding = *state.shortcuts;
    const int result = retain_handle(lua, binding, shortcut_metatable);
    if (result < 0) { binding.closed = true; binding.connection.stop(); }
    return result;
}

int shortcut_status(lua_State* lua) {
    const auto& binding = shortcut_handle(lua, true);
    const auto& status = binding.connection.status();
    static const char* phases[] = {"stopped", "connecting", "ready", "failed"};
    return connection_status(lua, binding.closed ? "closed" : phases[static_cast<unsigned>(status.phase)], status.error);
}

int shortcut_on_status(lua_State* lua) {
    auto& binding = shortcut_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int shortcut_on_event(lua_State* lua) {
    auto& binding = shortcut_handle(lua);
    replace_callback(lua, 2, binding.on_event, binding.closed);
    return 0;
}

int shortcut_restart(lua_State* lua) {
    auto& binding = shortcut_handle(lua, true);
    const auto result = start_shortcuts(invocation(lua));
    binding.closed = false; ++binding.generation; binding.events.clear(); binding.status_pending = true;
    return operation_result(lua, result);
}

int shortcut_close(lua_State* lua) {
    auto& binding = shortcut_handle(lua, true);
    binding.connection.stop(); binding.closed = true; ++binding.generation;
    binding.events.clear(); binding.status_pending = false;
    unref(lua, binding.on_status);
    unref(lua, binding.on_event); return 0;
}

}

void push_shortcut_event(lua_State* lua, const shortcuts::Event& event) {
    lua_createtable(lua, 0, 4);
    string_field(lua, "id", event.id); lua_pushboolean(lua, event.pressed); lua_setfield(lua, -2, "pressed");
    identifier(lua, event.seconds); lua_setfield(lua, -2, "seconds");
    lua_pushinteger(lua, event.nanoseconds); lua_setfield(lua, -2, "nanoseconds");
}

void register_shortcuts(lua_State* lua) {
    luaL_newmetatable(lua, shortcut_metatable);
    const luaL_Reg shortcut_methods[] = {{"status", guarded<shortcut_status>}, {"on_status", guarded<shortcut_on_status>},
        {"on_event", guarded<shortcut_on_event>}, {"restart", guarded<shortcut_restart>}, {"close", guarded<shortcut_close>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, shortcut_methods, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<shortcut_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks global shortcuts"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<shortcut_open>); lua_setfield(lua, -2, "shortcuts");
}

}
