#include "../runtime/invocation.hpp"

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char power_metatable[] = "hyprworks.upower";

PowerBinding& power_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<PowerBinding**>(luaL_checkudata(lua, 1, power_metatable));
    if (invocation(lua).closing)
        luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "UPower connection is closed");
    return binding;
}

int power_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing)
        return luaL_error(lua, "invocation is shutting down");
    if (!state.power) {
        state.power.emplace();
        const auto started = state.power->connection.start();
        state.power->status_pending = !started;
    }
    return retain_handle(lua, *state.power, power_metatable);
}

void push_device(lua_State* lua, const upower::Device& device) {
    lua_createtable(lua, 0, 19);
    string_field(lua, "path", device.path);
    string_field(lua, "vendor", device.vendor);
    string_field(lua, "model", device.model);
    string_field(lua, "icon_name", device.icon_name);
    for (const auto& [name, value] : {std::pair{"type", device.type}, {"state", device.state}, {"warning_level", device.warning_level}, {"battery_level", device.battery_level}}) {
        lua_pushinteger(lua, value); lua_setfield(lua, -2, name);
    }
    for (const auto& [name, value] : {std::pair{"present", device.present}, {"power_supply", device.power_supply}, {"online", device.online}, {"rechargeable", device.rechargeable}}) {
        lua_pushboolean(lua, value); lua_setfield(lua, -2, name);
    }
    for (const auto& [name, value] : {std::pair{"percentage", device.percentage}, {"capacity", device.capacity}, {"energy", device.energy}, {"energy_full", device.energy_full}, {"energy_rate", device.energy_rate}}) {
        lua_pushnumber(lua, value); lua_setfield(lua, -2, name);
    }
    lua_pushinteger(lua, device.time_to_empty); lua_setfield(lua, -2, "time_to_empty");
    lua_pushinteger(lua, device.time_to_full); lua_setfield(lua, -2, "time_to_full");
}

int power_snapshot(lua_State* lua) {
    const auto snapshot = power_handle(lua, true).connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 6);
        identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        lua_pushboolean(lua, snapshot->on_battery); lua_setfield(lua, -2, "on_battery");
        lua_pushboolean(lua, snapshot->lid_present); lua_setfield(lua, -2, "lid_present");
        lua_pushboolean(lua, snapshot->lid_closed); lua_setfield(lua, -2, "lid_closed");
        push_device(lua, snapshot->display); lua_setfield(lua, -2, "display");
        lua_createtable(lua, static_cast<int>(snapshot->devices.size()), 0);
        lua_Integer index = 1;
        for (const auto& device : snapshot->devices) {
            push_device(lua, device);
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "devices");
        return 1;
    }, 1);
}

int power_status(lua_State* lua) {
    const auto& binding = power_handle(lua, true);
    const auto status = binding.connection.status();
    const bool closed = binding.closed, has_snapshot = bool(binding.connection.snapshot());
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "synchronizing", "ready", "unavailable", "failed"};
        lua_createtable(lua, 0, 3);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]);
        lua_setfield(lua, -2, "phase");
        lua_pushboolean(lua, has_snapshot); lua_setfield(lua, -2, "has_snapshot");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int power_on_status(lua_State* lua) {
    auto& binding = power_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int power_on_change(lua_State* lua) {
    auto& binding = power_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int power_restart(lua_State* lua) {
    auto& binding = power_handle(lua, true);
    const auto result = binding.connection.start();
    binding.closed = false;
    ++binding.generation;
    binding.change_pending = binding.status_pending = true;
    return operation_result(lua, result);
}

int power_refresh(lua_State* lua) {
    power_handle(lua).connection.refresh();
    return 0;
}

int power_close(lua_State* lua) {
    auto& binding = power_handle(lua, true);
    binding.closed = true;
    ++binding.generation;
    binding.connection.stop();
    binding.change_pending = binding.status_pending = false;
    unref(lua, binding.on_change);
    unref(lua, binding.on_status);
    return 0;
}

}

int dispatch_power(lua_State* lua, PowerBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false);
    const bool change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    if (status && dispatch_callback(lua, binding, binding.on_status, generation, [](lua_State*) { return 0; }) < 0) return -1;
    if (change && dispatch_callback(lua, binding, binding.on_change, generation, [](lua_State*) { return 0; }) < 0) return -1;
    return 0;
}

void register_upower(lua_State* lua) {
    luaL_newmetatable(lua, power_metatable);
    const luaL_Reg power_methods[] = {
        {"snapshot", guarded<power_snapshot>}, {"status", guarded<power_status>},
        {"on_change", guarded<power_on_change>}, {"on_status", guarded<power_on_status>},
        {"refresh", guarded<power_refresh>}, {"restart", guarded<power_restart>}, {"close", guarded<power_close>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, power_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<power_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks UPower connection"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<power_open>); lua_setfield(lua, -2, "upower");
}

}
