#include "../runtime/invocation.hpp"

#include <type_traits>
#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char notification_metatable[] = "hyprworks.notifications";

NotificationBinding& notification_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<NotificationBinding**>(luaL_checkudata(lua, 1, notification_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "notification server is closed");
    return binding;
}

int notification_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.notifications) {
        notifications::Options options;
        if (lua_isnone(lua, 1)) lua_pushnil(lua); else lua_pushvalue(lua, 1);
        if (protected_call(lua, [&](lua_State* lua) {
            luaL_checktype(lua, 2, LUA_TTABLE);
            for (const auto& [name, target] : {std::pair{"name", &options.name}, {"vendor", &options.vendor}, {"version", &options.version}}) {
                field(lua, 2, name);
                std::size_t length; const char* text = check_string(lua, -1, &length);
                luaL_argcheck(lua, length <= 128, 2, "server information exceeds 128 bytes");
                target->assign(text, length);
                lua_pop(lua, 1);
            }
            field(lua, 2, "capabilities");
            if (!lua_isnil(lua, -1)) {
                luaL_checktype(lua, -1, LUA_TTABLE);
                const auto count = lua_rawlen(lua, -1);
                luaL_argcheck(lua, count <= 64, 2, "at most 64 notification capabilities");
                for (std::size_t i = 0; i < count; ++i) {
                    lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1));
                    std::size_t length; const char* text = check_string(lua, -1, &length);
                    luaL_argcheck(lua, length <= 128, 2, "capability exceeds 128 bytes");
                    options.capabilities.emplace_back(text, length); lua_pop(lua, 1);
                }
            }
            lua_pop(lua, 1);
            field(lua, 2, "default_timeout");
            if (!lua_isnil(lua, -1)) options.default_timeout = std::chrono::milliseconds(check_integer(lua, -1));
            lua_pop(lua, 1); return 0;
        }, 0, 0, 1) < 0) return -1;
        if (!state.notifications) {
            auto& binding = state.notifications.emplace();
            binding.options = std::move(options);
            const auto started = binding.server.start(binding.options);
            binding.status_pending = !started;
        }
    }
    return retain_handle(lua, *state.notifications, notification_metatable);
}

void push_notification(lua_State* lua, const notifications::Notification& notification) {
    lua_createtable(lua, 0, 8);
    lua_pushinteger(lua, notification.id); lua_setfield(lua, -2, "id");
    string_field(lua, "sender", notification.sender); string_field(lua, "app_name", notification.app_name);
    string_field(lua, "app_icon", notification.app_icon); string_field(lua, "summary", notification.summary); string_field(lua, "body", notification.body);
    if (notification.expires_at) {
        lua_pushinteger(lua, std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(*notification.expires_at - notifications::Clock::now()).count()));
        lua_setfield(lua, -2, "remaining");
    }
    lua_createtable(lua, static_cast<int>(notification.actions.size()), 0);
    lua_Integer index = 1;
    for (const auto& action : notification.actions) {
        lua_createtable(lua, 0, 2); string_field(lua, "key", action.key); string_field(lua, "label", action.label);
        lua_rawseti(lua, -2, index++);
    }
    lua_setfield(lua, -2, "actions");
    lua_createtable(lua, 0, static_cast<int>(notification.hints.size()));
    for (const auto& [key, hint] : notification.hints) {
        lua_pushlstring(lua, key.data(), key.size());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) lua_pushboolean(lua, value);
            else if constexpr (std::is_same_v<T, std::int64_t>) lua_pushinteger(lua, value);
            else if constexpr (std::is_same_v<T, std::uint64_t>) {
                if (value <= LUA_MAXINTEGER) lua_pushinteger(lua, static_cast<lua_Integer>(value)); else identifier(lua, value);
            } else if constexpr (std::is_same_v<T, double>) lua_pushnumber(lua, value);
            else if constexpr (std::is_same_v<T, std::string>) lua_pushlstring(lua, value.data(), value.size());
            else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) lua_pushlstring(lua, reinterpret_cast<const char*>(value.data()), value.size());
            else if constexpr (std::is_same_v<T, std::vector<std::string>>) push_strings(lua, value);
            else {
                lua_createtable(lua, 0, 6);
                for (const auto& [field, number] : {std::pair{"width", value.width}, {"height", value.height}, {"row_stride", value.row_stride}, {"channels", value.channels}}) {
                    lua_pushinteger(lua, number); lua_setfield(lua, -2, field);
                }
                lua_pushboolean(lua, value.alpha); lua_setfield(lua, -2, "alpha");
                lua_pushlstring(lua, reinterpret_cast<const char*>(value.pixels.data()), value.pixels.size()); lua_setfield(lua, -2, "pixels");
            }
        }, hint);
        lua_rawset(lua, -3);
    }
    lua_setfield(lua, -2, "hints");
}

int notification_get(lua_State* lua) {
    auto& binding = notification_handle(lua);
    const auto id = check_integer(lua, 2);
    std::optional<notifications::Notification> notification;
    if (id > 0 && id <= UINT32_MAX) {
        const auto& records = binding.server.notifications();
        const auto found = std::ranges::find(records, static_cast<std::uint32_t>(id), &notifications::Notification::id);
        if (found != records.end()) notification = *found;
    }
    return protected_call(lua, [&](lua_State* lua) { if (notification) push_notification(lua, *notification); else lua_pushnil(lua); return 1; }, 1);
}

int notification_snapshot(lua_State* lua) {
    const auto records = notification_handle(lua).server.notifications();
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, static_cast<int>(records.size()), 0);
        lua_Integer index = 1;
        for (const auto& record : records) { push_notification(lua, record); lua_rawseti(lua, -2, index++); }
        return 1;
    }, 1);
}

int notification_status(lua_State* lua) {
    const auto& binding = notification_handle(lua, true);
    const auto status = binding.server.status();
    const bool closed = binding.closed;
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "ready", "failed"};
        lua_createtable(lua, 0, 2);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int notification_on_status(lua_State* lua) {
    auto& binding = notification_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int notification_on_event(lua_State* lua) {
    auto& binding = notification_handle(lua);
    replace_callback(lua, 2, binding.on_event, binding.closed);
    return 0;
}

int notification_invoke(lua_State* lua) {
    const auto id = check_integer(lua, 2);
    luaL_argcheck(lua, id > 0 && id <= UINT32_MAX, 2, "invalid notification identifier");
    std::size_t action_length, token_length;
    const char* action = check_string(lua, 3, &action_length);
    token_length = 0;
    const char* token = lua_isnoneornil(lua, 4) ? "" : check_string(lua, 4, &token_length);
    luaL_argcheck(lua, action_length <= 16384 && token_length <= 16384, 3, "notification action or token exceeds 16 KiB");
    auto& binding = notification_handle(lua);
    const auto result = binding.server.invoke(static_cast<std::uint32_t>(id), std::string(action, action_length), std::string(token, token_length));
    return operation_result(lua, result);
}

int notification_dismiss(lua_State* lua) {
    const auto id = check_integer(lua, 2);
    luaL_argcheck(lua, id > 0 && id <= UINT32_MAX, 2, "invalid notification identifier");
    const auto reason = check_integer(lua, 3);
    luaL_argcheck(lua, reason >= 1 && reason <= 4, 3, "close reason must be 1..4");
    auto& binding = notification_handle(lua);
    const auto result = binding.server.close(static_cast<std::uint32_t>(id), static_cast<notifications::CloseReason>(reason));
    return operation_result(lua, result);
}

int notification_restart(lua_State* lua) {
    auto& binding = notification_handle(lua, true);
    const auto before = binding.server.status();
    const auto result = binding.server.start(binding.options);
    binding.closed = false; binding.status_pending = true;
    if (result || before != binding.server.status()) { ++binding.generation; binding.events.clear(); }
    return operation_result(lua, result);
}

int notification_close(lua_State* lua) {
    auto& binding = notification_handle(lua, true);
    binding.server.stop(); binding.closed = true; binding.status_pending = false; ++binding.generation; binding.events.clear();
    unref(lua, binding.on_status);
    unref(lua, binding.on_event); return 0;
}

}

int dispatch_notifications(lua_State* lua, NotificationBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false);
    const auto events = std::exchange(binding.events, {});
    const auto generation = binding.generation;
    if (status && dispatch_callback(lua, binding, binding.on_status, generation, [](lua_State*) { return 0; }) < 0) return -1;
    for (const auto& event : events) {
        if (dispatch_callback(lua, binding, binding.on_event, generation, [&](lua_State* lua) {
            static const char* kinds[] = {"received", "closed", "action"};
            lua_createtable(lua, 0, 5);
            lua_pushstring(lua, kinds[static_cast<unsigned>(event.kind)]); lua_setfield(lua, -2, "kind");
            lua_pushinteger(lua, event.id); lua_setfield(lua, -2, "id");
            lua_pushinteger(lua, static_cast<unsigned>(event.reason)); lua_setfield(lua, -2, "reason");
            lua_pushboolean(lua, event.replaced); lua_setfield(lua, -2, "replaced");
            if (!event.action.empty()) string_field(lua, "action", event.action);
            return 1;
        }) < 0) return -1;
    }
    return 0;
}

void register_notifications(lua_State* lua) {
    luaL_newmetatable(lua, notification_metatable);
    const luaL_Reg notification_methods[] = {
        {"get", guarded<notification_get>}, {"snapshot", guarded<notification_snapshot>}, {"status", guarded<notification_status>},
        {"on_status", guarded<notification_on_status>}, {"on_event", guarded<notification_on_event>},
        {"dismiss", guarded<notification_dismiss>}, {"invoke", guarded<notification_invoke>},
        {"restart", guarded<notification_restart>}, {"close", guarded<notification_close>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, notification_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<notification_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks notification server"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<notification_open>); lua_setfield(lua, -2, "notifications");
}

}
