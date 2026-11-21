#include "../runtime/invocation.hpp"

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char tray_metatable[] = "hyprworks.tray", menu_metatable[] = "hyprworks.dbusmenu";

TrayBinding& tray_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<TrayBinding**>(luaL_checkudata(lua, 1, tray_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "connection is closed");
    return binding;
}

MenuBinding& menu_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<MenuBinding**>(luaL_checkudata(lua, 1, menu_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "connection is closed");
    return binding;
}

int tray_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    bool own = false;
    if (!lua_isnoneornil(lua, 1)) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        lua_getfield(lua, 1, "own_watcher");
        if (!lua_isnil(lua, -1)) luaL_checktype(lua, -1, LUA_TBOOLEAN);
        own = lua_toboolean(lua, -1); lua_pop(lua, 1);
    }
    if (!state.tray) {
        state.tray.emplace(); state.tray->own_watcher = own;
        const auto started = state.tray->connection.start(own); state.tray->status_pending = !started;
    }
    return retain_handle(lua, *state.tray, tray_metatable);
}

int menu_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    luaL_checktype(lua, 1, LUA_TTABLE);
    lua_getfield(lua, 1, "owner");
    std::size_t owner_length, path_length;
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    lua_getfield(lua, 1, "path");
    const char* path = luaL_checklstring(lua, -1, &path_length);
    luaL_argcheck(lua, owner_length <= 255 && path_length <= 16384, 1, "invalid menu target");
    if (state.menus.size() >= 32) return luaL_error(lua, "menu handle limit reached");
    auto created = std::make_unique<MenuBinding>();
    created->target = {std::string(owner, owner_length), std::string(path, path_length)};
    auto& binding = *created;
    const auto started = binding.connection.start(binding.target); binding.status_pending = !started;
    state.menus.push_back(std::move(created));
    const int result = retain_handle(lua, binding, menu_metatable);
    if (result < 0) { binding.closed = true; binding.connection.stop(); }
    return result;
}

void push_tray_result(lua_State* lua, const tray::RequestResult& result) {
    static const char* outcomes[] = {"acknowledged", "rejected", "failed", "cancelled"};
    lua_createtable(lua, 0, 4); identifier(lua, result.id); lua_setfield(lua, -2, "id");
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result.outcome)]); lua_setfield(lua, -2, "outcome");
    lua_pushboolean(lua, result.may_have_executed); lua_setfield(lua, -2, "may_have_executed");
    if (!result.error.empty()) string_field(lua, "error", result.error);
}

void push_menu_result(lua_State* lua, const dbusmenu::RequestResult& result) {
    static const char* outcomes[] = {"acknowledged", "rejected", "failed", "cancelled"};
    lua_createtable(lua, 0, 4); identifier(lua, result.id); lua_setfield(lua, -2, "id");
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result.outcome)]); lua_setfield(lua, -2, "outcome");
    lua_pushboolean(lua, result.may_have_executed); lua_setfield(lua, -2, "may_have_executed");
    if (!result.error.empty()) string_field(lua, "error", result.error);
}

void push_tray_icon(lua_State* lua, const tray::Icon& icon) {
    lua_createtable(lua, 0, 2); string_field(lua, "name", icon.name);
    lua_createtable(lua, static_cast<int>(icon.pixmaps.size()), 0);
    lua_Integer index = 1;
    for (const auto& image : icon.pixmaps) {
        lua_createtable(lua, 0, 6);
        for (const auto& [name, value] : {std::pair{"width", image.width}, {"height", image.height}, {"row_stride", image.width * 4}, {"channels", 4}}) {
            lua_pushinteger(lua, value); lua_setfield(lua, -2, name);
        }
        lua_pushboolean(lua, true); lua_setfield(lua, -2, "alpha");
        lua_pushlstring(lua, reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size()); lua_setfield(lua, -2, "pixels");
        lua_rawseti(lua, -2, index++);
    }
    lua_setfield(lua, -2, "pixmaps");
}

int tray_snapshot(lua_State* lua) {
    const auto snapshot = tray_handle(lua).connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 2); identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        lua_createtable(lua, static_cast<int>(snapshot->items.size()), 0);
        lua_Integer index = 1;
        for (const auto& item : snapshot->items) {
            lua_createtable(lua, 0, 20);
            for (const auto& [name, value] : {std::pair{"service", &item.key.service}, {"owner", &item.key.owner}, {"path", &item.key.path},
                    {"id", &item.id}, {"category", &item.category}, {"title", &item.title}, {"status", &item.status}, {"icon_theme_path", &item.icon_theme_path},
                    {"menu_path", &item.menu_path}, {"tooltip_title", &item.tooltip_title}, {"tooltip_body", &item.tooltip_body}, {"attention_movie", &item.attention_movie}}) string_field(lua, name, *value);
            lua_pushboolean(lua, item.available); lua_setfield(lua, -2, "available");
            lua_pushboolean(lua, item.item_is_menu); lua_setfield(lua, -2, "item_is_menu");
            if (!item.error.empty()) string_field(lua, "error", item.error);
            for (const auto& [name, icon] : {std::pair{"icon", &item.icon}, {"attention_icon", &item.attention_icon}, {"overlay_icon", &item.overlay_icon}, {"tooltip_icon", &item.tooltip_icon}}) {
                push_tray_icon(lua, *icon); lua_setfield(lua, -2, name);
            }
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "items"); return 1;
    }, 1);
}

int menu_snapshot(lua_State* lua) {
    const auto snapshot = menu_handle(lua).connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 7); identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        lua_pushinteger(lua, snapshot->remote_revision); lua_setfield(lua, -2, "remote_revision");
        lua_pushinteger(lua, snapshot->version); lua_setfield(lua, -2, "version");
        string_field(lua, "text_direction", snapshot->text_direction); string_field(lua, "status", snapshot->status);
        push_strings(lua, snapshot->icon_theme_paths);
        lua_setfield(lua, -2, "icon_theme_paths");
        lua_createtable(lua, static_cast<int>(snapshot->items.size()), 0);
        lua_Integer index = 1;
        for (const auto& item : snapshot->items) {
            lua_createtable(lua, 0, 13);
            for (const auto& [name, value] : {std::pair{"id", item.id}, {"parent", item.parent}, {"toggle_state", item.toggle_state}}) { lua_pushinteger(lua, value); lua_setfield(lua, -2, name); }
            for (const auto& [name, value] : {std::pair{"label", &item.label}, {"type", &item.type}, {"icon_name", &item.icon_name}, {"toggle_type", &item.toggle_type},
                    {"children_display", &item.children_display}, {"disposition", &item.disposition}}) string_field(lua, name, *value);
            lua_pushboolean(lua, item.enabled); lua_setfield(lua, -2, "enabled"); lua_pushboolean(lua, item.visible); lua_setfield(lua, -2, "visible");
            lua_pushlstring(lua, reinterpret_cast<const char*>(item.icon_data.data()), item.icon_data.size()); lua_setfield(lua, -2, "icon_data");
            lua_createtable(lua, static_cast<int>(item.shortcuts.size()), 0);
            lua_Integer shortcut_index = 1;
            for (const auto& shortcut : item.shortcuts) {
                push_strings(lua, shortcut);
                lua_rawseti(lua, -2, shortcut_index++);
            }
            lua_setfield(lua, -2, "shortcuts"); lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "items"); return 1;
    }, 1);
}

int tray_status(lua_State* lua) {
    const auto& binding = tray_handle(lua, true);
    const auto status = binding.connection.status(); const bool closed = binding.closed, snapshot = bool(binding.connection.snapshot());
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "synchronizing", "ready", "unavailable", "failed"};
        lua_createtable(lua, 0, 3); lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        lua_pushboolean(lua, snapshot); lua_setfield(lua, -2, "has_snapshot");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int menu_status(lua_State* lua) {
    const auto& binding = menu_handle(lua, true);
    const auto status = binding.connection.status(); const bool closed = binding.closed, snapshot = bool(binding.connection.snapshot());
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "synchronizing", "ready", "unavailable", "failed"};
        lua_createtable(lua, 0, 3); lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        lua_pushboolean(lua, snapshot); lua_setfield(lua, -2, "has_snapshot");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int tray_on_status(lua_State* lua) {
    auto& binding = tray_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int tray_on_change(lua_State* lua) {
    auto& binding = tray_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int tray_on_result(lua_State* lua) {
    auto& binding = tray_handle(lua);
    replace_callback(lua, 2, binding.on_result, binding.closed);
    return 0;
}

int menu_on_status(lua_State* lua) {
    auto& binding = menu_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int menu_on_change(lua_State* lua) {
    auto& binding = menu_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int menu_on_result(lua_State* lua) {
    auto& binding = menu_handle(lua);
    replace_callback(lua, 2, binding.on_result, binding.closed);
    return 0;
}

int menu_on_activation(lua_State* lua) {
    auto& binding = menu_handle(lua);
    replace_callback(lua, 2, binding.on_activation, binding.closed);
    return 0;
}

int tray_refresh(lua_State* lua) {
    return operation_result(lua, tray_handle(lua).connection.refresh());
}

int menu_refresh(lua_State* lua) {
    return operation_result(lua, menu_handle(lua).connection.refresh());
}

int tray_restart(lua_State* lua) {
    auto& binding = tray_handle(lua, true);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    const auto result = binding.connection.start(binding.own_watcher);
    binding.closed = false; binding.status_pending = true;
    if (result) { ++binding.generation; binding.change_pending = true; }
    return operation_result(lua, result);
}

int menu_restart(lua_State* lua) {
    auto& binding = menu_handle(lua, true);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    dbusmenu::Target target = binding.target;
    if (!lua_isnoneornil(lua, 2)) {
        lua_pushvalue(lua, 2);
        if (protected_call(lua, [&](lua_State* lua) {
            luaL_checktype(lua, 2, LUA_TTABLE);
            for (const auto& [name, value] : {std::pair{"owner", &target.owner}, {"path", &target.path}}) {
                lua_getfield(lua, 2, name);
                std::size_t size; const char* text = luaL_checklstring(lua, -1, &size);
                luaL_argcheck(lua, size <= 16384, 2, "menu target exceeds 16 KiB");
                value->assign(text, size); lua_pop(lua, 1);
            }
            return 0;
        }, 0, 0, 1) < 0) return -1;
    }
    const auto result = binding.connection.start(target);
    if (result) binding.target = std::move(target);
    binding.closed = false; binding.status_pending = true;
    if (result) { ++binding.generation; binding.change_pending = true; }
    return operation_result(lua, result);
}

int tray_close(lua_State* lua) {
    auto& binding = tray_handle(lua, true);
    auto result = binding.connection.stop(); if (binding.result) result = std::exchange(binding.result, {});
    binding.closed = true; ++binding.generation; binding.status_pending = binding.change_pending = false;
    for (auto* reference : {&binding.on_status, &binding.on_change, &binding.on_result}) unref(lua, *reference);
    return protected_call(lua, [&](lua_State* lua) { if (result) push_tray_result(lua, *result); else lua_pushnil(lua); return 1; }, 1);
}

int menu_close(lua_State* lua) {
    auto& binding = menu_handle(lua, true);
    auto result = binding.connection.stop(); if (binding.result) result = std::exchange(binding.result, {});
    binding.closed = true; ++binding.generation; binding.status_pending = binding.change_pending = false;
    binding.activations.clear();
    for (auto* reference : {&binding.on_status, &binding.on_change, &binding.on_result, &binding.on_activation}) unref(lua, *reference);
    return protected_call(lua, [&](lua_State* lua) { if (result) push_menu_result(lua, *result); else lua_pushnil(lua); return 1; }, 1);
}

int tray_action(lua_State* lua) {
    auto& binding = tray_handle(lua);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_settop(lua, 5);
    const char* parts[3]; std::size_t sizes[3];
    unsigned i = 0;
    for (const auto* field : {"service", "owner", "path"}) {
        lua_getfield(lua, 2, field); parts[i] = luaL_checklstring(lua, -1, &sizes[i]);
        luaL_argcheck(lua, sizes[i] <= (i == 2 ? 16384U : 255U), 2, "invalid tray key"); ++i;
    }
    static const char* actions[] = {"activate", "secondary_activate", "context_menu", nullptr};
    const auto action = luaL_checkoption(lua, 3, nullptr, actions);
    const auto x = luaL_optinteger(lua, 4, 0), y = luaL_optinteger(lua, 5, 0);
    luaL_argcheck(lua, x >= INT32_MIN && x <= INT32_MAX && y >= INT32_MIN && y <= INT32_MAX, 3, "coordinate or delta exceeds int32");
    if (binding.closed) return luaL_error(lua, "tray connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    const tray::ItemKey key{std::string(parts[0], sizes[0]), std::string(parts[1], sizes[1]), std::string(parts[2], sizes[2])};
    const auto result = binding.connection.action(key, static_cast<tray::Action>(action), static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    return request_result(lua, result);
}

int tray_scroll(lua_State* lua) {
    auto& binding = tray_handle(lua);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_settop(lua, 4);
    const char* parts[3]; std::size_t sizes[3];
    unsigned i = 0;
    for (const auto* field : {"service", "owner", "path"}) {
        lua_getfield(lua, 2, field); parts[i] = luaL_checklstring(lua, -1, &sizes[i]);
        luaL_argcheck(lua, sizes[i] <= (i == 2 ? 16384U : 255U), 2, "invalid tray key"); ++i;
    }
    const auto delta = luaL_checkinteger(lua, 3);
    static const char* directions[] = {"vertical", "horizontal", nullptr};
    const bool horizontal = luaL_checkoption(lua, 4, "vertical", directions) == 1;
    luaL_argcheck(lua, delta >= INT32_MIN && delta <= INT32_MAX, 3, "coordinate or delta exceeds int32");
    if (binding.closed) return luaL_error(lua, "tray connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    const tray::ItemKey key{std::string(parts[0], sizes[0]), std::string(parts[1], sizes[1]), std::string(parts[2], sizes[2])};
    const auto result = binding.connection.scroll(key, static_cast<std::int32_t>(delta), horizontal);
    return request_result(lua, result);
}

int menu_about_to_show(lua_State* lua) {
    auto& binding = menu_handle(lua);
    const auto id = luaL_optinteger(lua, 2, 0);
    luaL_argcheck(lua, id >= 0 && id <= INT32_MAX, 2, "invalid menu item ID");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    const auto result = binding.connection.about_to_show(static_cast<std::int32_t>(id));
    return request_result(lua, result);
}

int menu_event(lua_State* lua) {
    auto& binding = menu_handle(lua);
    const auto id = luaL_optinteger(lua, 2, 0);
    luaL_argcheck(lua, id >= 0 && id <= INT32_MAX, 2, "invalid menu item ID");
    std::size_t length;
    const char* event = luaL_checklstring(lua, 3, &length);
    const auto timestamp = luaL_optinteger(lua, 4, 0);
    luaL_argcheck(lua, length <= 128 && timestamp >= 0 && timestamp <= UINT32_MAX, 3, "invalid menu event or timestamp");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "a result awaits delivery"); return 2; }
    const auto result = binding.connection.event(static_cast<std::int32_t>(id), std::string(event, length), static_cast<std::uint32_t>(timestamp));
    return request_result(lua, result);
}

}

int dispatch_tray(lua_State* lua, TrayBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false), change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    for (const auto event : {Event::status, Event::change, Event::result}) {
        if (binding.closed || generation != binding.generation || invocation(lua).requests.load(std::memory_order_relaxed)) break;
        if (!(event == Event::status ? status : event == Event::change ? change : binding.result.has_value())) continue;
        auto result = event == Event::result ? std::exchange(binding.result, {}) : std::nullopt;
        const int& callback = event == Event::status ? binding.on_status : event == Event::change ? binding.on_change : binding.on_result;
        if (dispatch_callback(lua, binding, callback, generation, [&](lua_State* lua) {
            if (result) push_tray_result(lua, *result);
            return result ? 1 : 0;
        }) < 0) return -1;
    }
    return 0;
}

int dispatch_menu(lua_State* lua, MenuBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false), change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    const auto activations = std::exchange(binding.activations, {});
    for (const auto event : {Event::status, Event::change, Event::result}) {
        if (binding.closed || generation != binding.generation || invocation(lua).requests.load(std::memory_order_relaxed)) break;
        if (!(event == Event::status ? status : event == Event::change ? change : binding.result.has_value())) continue;
        auto result = event == Event::result ? std::exchange(binding.result, {}) : std::nullopt;
        const int& callback = event == Event::status ? binding.on_status : event == Event::change ? binding.on_change : binding.on_result;
        if (dispatch_callback(lua, binding, callback, generation, [&](lua_State* lua) {
            if (result) push_menu_result(lua, *result);
            return result ? 1 : 0;
        }) < 0) return -1;
    }
    for (const auto& activation : activations) {
        if (dispatch_callback(lua, binding, binding.on_activation, generation, [&](lua_State* lua) {
            lua_createtable(lua, 0, 2); lua_pushinteger(lua, activation.id); lua_setfield(lua, -2, "id");
            lua_pushinteger(lua, activation.timestamp); lua_setfield(lua, -2, "timestamp");
            return 1;
        }) < 0) return -1;
    }
    return 0;
}

void register_tray(lua_State* lua) {
    const luaL_Reg tray_methods[] = {
        {"snapshot", guarded<tray_snapshot>}, {"status", guarded<tray_status>},
        {"on_status", guarded<tray_on_status>}, {"on_change", guarded<tray_on_change>}, {"on_result", guarded<tray_on_result>},
        {"refresh", guarded<tray_refresh>}, {"restart", guarded<tray_restart>}, {"close", guarded<tray_close>},
        {"action", guarded<tray_action>}, {"scroll", guarded<tray_scroll>}, {nullptr, nullptr}
    };
    const luaL_Reg menu_methods[] = {
        {"snapshot", guarded<menu_snapshot>}, {"status", guarded<menu_status>},
        {"on_status", guarded<menu_on_status>}, {"on_change", guarded<menu_on_change>}, {"on_result", guarded<menu_on_result>},
        {"on_activation", guarded<menu_on_activation>}, {"refresh", guarded<menu_refresh>},
        {"restart", guarded<menu_restart>}, {"close", guarded<menu_close>},
        {"about_to_show", guarded<menu_about_to_show>}, {"event", guarded<menu_event>}, {nullptr, nullptr}
    };
    luaL_newmetatable(lua, tray_metatable); luaL_setfuncs(lua, tray_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<tray_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks protocol connection"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    luaL_newmetatable(lua, menu_metatable); luaL_setfuncs(lua, menu_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<menu_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks protocol connection"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<tray_open>); lua_setfield(lua, -2, "tray");
    lua_pushcfunction(lua, guarded<menu_open>); lua_setfield(lua, -2, "dbusmenu");
}

}
