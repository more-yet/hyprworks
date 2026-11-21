#pragma once

#include <lua.hpp>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace hyprworks::lua {

template<int (*Function)(lua_State*)>
int operation_call(lua_State* lua) {
    char message[1024];
    int result = 0; bool failed = false;
    try { result = Function(lua); }
    catch (const std::exception& error) { std::snprintf(message, sizeof(message), "%s", error.what()); failed = true; }
    if (failed) { lua_pushnil(lua); lua_pushstring(lua, message); return 2; }
    return result < 0 ? lua_error(lua) : result;
}

inline void string_field(lua_State* lua, const char* name, std::string_view value) {
    lua_pushlstring(lua, value.data(), value.size());
    lua_setfield(lua, -2, name);
}

inline void push_strings(lua_State* lua, std::span<const std::string> values) {
    lua_createtable(lua, static_cast<int>(values.size()), 0);
    lua_Integer index = 1;
    for (const auto& value : values) {
        lua_pushlstring(lua, value.data(), value.size());
        lua_rawseti(lua, -2, index++);
    }
}

inline void identifier(lua_State* lua, std::uint64_t id) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), id);
    lua_pushlstring(lua, buffer, static_cast<std::size_t>(result.ptr - buffer));
}

inline void unref(lua_State* lua, int& reference) noexcept {
    if (reference != LUA_NOREF)
        luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(reference, LUA_NOREF));
}

inline bool replace_callback(lua_State* lua, int argument, int& reference, const bool& closed) {
    if (!lua_isnoneornil(lua, argument)) luaL_checktype(lua, argument, LUA_TFUNCTION);
    if (lua_isnone(lua, argument)) lua_pushnil(lua); else lua_pushvalue(lua, argument);
    const int replacement = luaL_ref(lua, LUA_REGISTRYINDEX);
    if (closed) {
        luaL_unref(lua, LUA_REGISTRYINDEX, replacement);
        luaL_error(lua, "handle closed while setting callback");
    }
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(reference, replacement));
    return replacement >= 0;
}

inline void field(lua_State* lua, int table, const char* name) {
    table = lua_absindex(lua, table);
    lua_pushstring(lua, name);
    lua_rawget(lua, table);
}

inline lua_Integer integer_field(lua_State* lua, int table, const char* name, lua_Integer fallback,
                          lua_Integer minimum = 0, lua_Integer maximum = 16384) {
    field(lua, table, name);
    const auto value = luaL_optinteger(lua, -1, fallback);
    if (value < minimum || value > maximum)
        luaL_error(lua, "%s is out of range", name);
    lua_pop(lua, 1);
    return value;
}

inline bool boolean_field(lua_State* lua, int table, const char* name, bool fallback) {
    field(lua, table, name);
    if (!lua_isnil(lua, -1))
        luaL_checktype(lua, -1, LUA_TBOOLEAN);
    const bool value = lua_isnil(lua, -1) ? fallback : lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    return value;
}

inline bool text_field(lua_State* lua, int table, const char* name, std::string& value, std::size_t limit = 16384) {
    field(lua, table, name);
    bool changed = false;
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TSTRING);
        std::size_t size;
        const char* text = lua_tolstring(lua, -1, &size);
        if (size > limit || std::string_view(text, size).contains('\0'))
            luaL_error(lua, "%s is too long or contains NUL", name);
        changed = value != std::string_view(text, size);
        if (changed) value.assign(text, size);
    }
    lua_pop(lua, 1);
    return changed;
}

inline int enum_field(lua_State* lua, int table, const char* name, const char* fallback, const char* const* choices) {
    field(lua, table, name);
    const int value = luaL_checkoption(lua, -1, fallback, choices);
    lua_pop(lua, 1);
    return value;
}

inline void callback_field(lua_State* lua, int table, const char* name, int& reference) {
    field(lua, table, name);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return;
    }
    luaL_checktype(lua, -1, LUA_TFUNCTION);
    reference = luaL_ref(lua, LUA_REGISTRYINDEX);
}

inline char interrupted;

/// Translate native failures after C++ locals unwind. Lua calls inside Function
/// need protected_call whenever their longjmp could bypass an owning local.
template<int (*Function)(lua_State*)>
int guarded(lua_State* lua) {
    char message[1024];
    int result = 0;
    bool native_error = false;
    try {
        result = Function(lua);
    } catch (const std::exception& error) {
        std::snprintf(message, sizeof(message), "%s", error.what());
        native_error = true;
    } catch (...) {
        std::snprintf(message, sizeof(message), "unexpected native exception");
        native_error = true;
    }
    if (native_error)
        return luaL_error(lua, "%s", message);
    return result < 0 ? lua_error(lua) : result;
}

template<typename Function>
int call_function(lua_State* lua) {
    return (*static_cast<Function*>(lua_touserdata(lua, 1)))(lua);
}

/// Keep owning locals in the caller and borrow them in Function. A negative
/// result leaves the Lua error on the stack for guarded to propagate.
template<typename Function>
int protected_call(lua_State* lua, Function function, int results,
                   int error_handler = 0, int extra_arguments = 0) {
    if (!lua_checkstack(lua, 3))
        throw std::runtime_error("cannot grow Lua stack");
    lua_pushcfunction(lua, guarded<call_function<Function>>);
    lua_insert(lua, -1 - extra_arguments);
    lua_pushlightuserdata(lua, &function);
    lua_insert(lua, -1 - extra_arguments);
    return lua_pcall(lua, 1 + extra_arguments, results, error_handler) == LUA_OK ? results : -1;
}

/// Binding storage outlives Lua. LUA_REFNIL reserves construction while Lua
/// allocation can run finalizers that reenter the constructor.
template<typename Binding>
int retain_handle(lua_State* lua, Binding& binding, const char* metatable) {
    if (binding.handle >= 0) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
        return 1;
    }
    if (binding.handle == LUA_REFNIL)
        throw std::runtime_error("handle is being opened");
    const int status = protected_call(lua, [&](lua_State* lua) {
        binding.handle = LUA_REFNIL;
        *static_cast<Binding**>(lua_newuserdatauv(lua, sizeof(Binding*), 0)) = &binding;
        luaL_setmetatable(lua, metatable);
        lua_pushvalue(lua, -1);
        binding.handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }, 1);
    if (status < 0) binding.handle = LUA_NOREF;
    return status;
}

inline int traceback(lua_State* lua) {
    if (lua_touserdata(lua, 1) == &interrupted) {
        lua_pushvalue(lua, 1);
        return 1;
    }
    const char* message = luaL_tolstring(lua, 1, nullptr);
    luaL_traceback(lua, lua, message, 1);
    return 1;
}

inline int operation_result(lua_State* lua, const std::expected<void, std::string>& result) {
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushboolean(lua, true); return 1; }
        lua_pushnil(lua);
        lua_pushlstring(lua, result.error().data(), result.error().size());
        return 2;
    }, 2);
}

inline int request_result(lua_State* lua, const std::expected<std::uint64_t, std::string>& result) {
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua);
        lua_pushlstring(lua, result.error().data(), result.error().size());
        return 2;
    }, 2);
}

}
