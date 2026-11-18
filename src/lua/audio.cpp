#include "invocation.hpp"

#include <array>
#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char audio_metatable[] = "hyprworks.audio";

AudioBinding& audio_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<AudioBinding**>(luaL_checkudata(lua, 1, audio_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "audio connection is closed");
    return binding;
}

int audio_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.audio) {
        state.audio.emplace();
        const auto started = state.audio->connection.start();
        state.audio->status_pending = !started;
    }
    auto& binding = *state.audio;
    if (binding.opening) return luaL_error(lua, "audio handle is being opened");
    const int status = protected_call(lua, [&](lua_State* lua) {
        if (binding.handle >= 0) { lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle); return 1; }
        binding.opening = true;
        *static_cast<AudioBinding**>(lua_newuserdatauv(lua, sizeof(AudioBinding*), 0)) = &binding;
        luaL_setmetatable(lua, audio_metatable);
        lua_pushvalue(lua, -1);
        binding.handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }, 1);
    binding.opening = false;
    return status;
}

void push_audio_devices(lua_State* lua, const std::vector<audio::Device>& devices) {
    lua_createtable(lua, static_cast<int>(devices.size()), 0);
    lua_Integer index = 1;
    for (const auto& device : devices) {
        lua_createtable(lua, 0, 8);
        string_field(lua, "name", device.name);
        string_field(lua, "description", device.description);
        string_field(lua, "icon_name", device.icon_name);
        lua_pushinteger(lua, device.index); lua_setfield(lua, -2, "index");
        static const char* states[] = {"unknown", "running", "idle", "suspended"};
        lua_pushstring(lua, states[static_cast<unsigned>(device.state)]); lua_setfield(lua, -2, "state");
        lua_pushboolean(lua, device.mute); lua_setfield(lua, -2, "mute");
        lua_pushboolean(lua, device.monitor); lua_setfield(lua, -2, "monitor");
        lua_createtable(lua, static_cast<int>(device.channels.size()), 0);
        lua_Integer channel_index = 1;
        for (const auto& channel : device.channels) {
            lua_createtable(lua, 0, 2);
            string_field(lua, "position", channel.position);
            lua_pushnumber(lua, channel.volume); lua_setfield(lua, -2, "volume");
            lua_rawseti(lua, -2, channel_index++);
        }
        lua_setfield(lua, -2, "channels");
        lua_rawseti(lua, -2, index++);
    }
}

int audio_snapshot(lua_State* lua) {
    const auto snapshot = audio_handle(lua).connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 5);
        identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        string_field(lua, "default_output", snapshot->default_output);
        string_field(lua, "default_input", snapshot->default_input);
        push_audio_devices(lua, snapshot->outputs); lua_setfield(lua, -2, "outputs");
        push_audio_devices(lua, snapshot->inputs); lua_setfield(lua, -2, "inputs");
        return 1;
    }, 1);
}

int audio_status(lua_State* lua) {
    const auto& binding = audio_handle(lua, true);
    const auto status = binding.connection.status();
    const bool closed = binding.closed, has_snapshot = binding.connection.snapshot().has_value();
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "synchronizing", "ready", "failed"};
        lua_createtable(lua, 0, 3);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]); lua_setfield(lua, -2, "phase");
        lua_pushboolean(lua, has_snapshot); lua_setfield(lua, -2, "has_snapshot");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int audio_on_status(lua_State* lua) {
    auto& binding = audio_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int audio_on_change(lua_State* lua) {
    auto& binding = audio_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int audio_on_result(lua_State* lua) {
    auto& binding = audio_handle(lua);
    replace_callback(lua, 2, binding.on_result, binding.closed);
    return 0;
}

void push_audio_result(lua_State* lua, const audio::RequestResult& result) {
    static const char* outcomes[] = {"acknowledged", "rejected", "failed", "cancelled"};
    lua_createtable(lua, 0, 4);
    identifier(lua, result.id); lua_setfield(lua, -2, "id");
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result.outcome)]); lua_setfield(lua, -2, "outcome");
    lua_pushboolean(lua, result.may_have_executed); lua_setfield(lua, -2, "may_have_executed");
    if (!result.error.empty()) string_field(lua, "error", result.error);
}

int audio_restart(lua_State* lua) {
    auto& binding = audio_handle(lua, true);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an audio result awaits delivery"); return 2; }
    const auto before = binding.connection.status();
    const bool had_snapshot = binding.connection.snapshot().has_value();
    auto result = binding.connection.start();
    binding.closed = false;
    if (result || before != binding.connection.status()) {
        ++binding.generation;
        binding.status_pending = true;
    }
    binding.change_pending |= had_snapshot && !binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushboolean(lua, true); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int audio_refresh(lua_State* lua) {
    auto result = audio_handle(lua).connection.refresh();
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushboolean(lua, true); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int audio_set_volume(lua_State* lua) {
    auto& binding = audio_handle(lua);
    static const char* directions[] = {"output", "input", nullptr};
    const auto direction = luaL_checkoption(lua, 2, nullptr, directions) == 0 ? audio::Direction::output : audio::Direction::input;
    std::size_t length = 0;
    const auto* name = luaL_checklstring(lua, 3, &length);
    std::array<double, 32> volumes{};
    luaL_checktype(lua, 4, LUA_TTABLE);
    const auto count = lua_rawlen(lua, 4);
    luaL_argcheck(lua, count > 0 && count <= volumes.size(), 4, "expected 1..32 channel volumes");
    for (std::size_t i = 0; i < count; ++i) {
        lua_rawgeti(lua, 4, static_cast<lua_Integer>(i + 1));
        volumes[i] = luaL_checknumber(lua, -1);
        lua_pop(lua, 1);
    }
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an audio result awaits delivery"); return 2; }
    const auto result = binding.connection.set_volume(direction, std::string(name, length), {volumes.data(), count});
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int audio_set_mute(lua_State* lua) {
    auto& binding = audio_handle(lua);
    static const char* directions[] = {"output", "input", nullptr};
    const auto direction = luaL_checkoption(lua, 2, nullptr, directions) == 0 ? audio::Direction::output : audio::Direction::input;
    std::size_t length;
    const char* name = luaL_checklstring(lua, 3, &length);
    luaL_checktype(lua, 4, LUA_TBOOLEAN);
    const bool mute = lua_toboolean(lua, 4);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an audio result awaits delivery"); return 2; }
    const auto result = binding.connection.set_mute(direction, std::string(name, length), mute);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int audio_set_default(lua_State* lua) {
    auto& binding = audio_handle(lua);
    static const char* directions[] = {"output", "input", nullptr};
    const auto direction = luaL_checkoption(lua, 2, nullptr, directions) == 0 ? audio::Direction::output : audio::Direction::input;
    std::size_t length;
    const char* name = luaL_checklstring(lua, 3, &length);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an audio result awaits delivery"); return 2; }
    const auto result = binding.connection.set_default(direction, std::string(name, length));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int audio_close(lua_State* lua) {
    auto& binding = audio_handle(lua, true);
    auto result = binding.connection.stop();
    if (binding.result) result = std::exchange(binding.result, {});
    binding.closed = true;
    ++binding.generation;
    binding.change_pending = binding.status_pending = false;
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_change, LUA_NOREF));
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_status, LUA_NOREF));
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_result, LUA_NOREF));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) push_audio_result(lua, *result); else lua_pushnil(lua);
        return 1;
    }, 1);
}

}

int dispatch_audio(lua_State* lua, AudioBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false);
    const bool change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    for (const auto kind : {Event::status, Event::change, Event::result}) {
        if (binding.closed || generation != binding.generation || invocation(lua).quit_requested.load(std::memory_order_relaxed)) break;
        if (!(kind == Event::status ? status : kind == Event::change ? change : binding.result.has_value())) continue;
        auto result = kind == Event::result ? std::exchange(binding.result, {}) : std::nullopt;
        const int reference = kind == Event::status ? binding.on_status : kind == Event::change ? binding.on_change : binding.on_result;
        if (reference < 0) continue;
        lua_pushcfunction(lua, traceback);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
            if (result) push_audio_result(lua, *result);
            const int current = kind == Event::status ? binding.on_status : kind == Event::change ? binding.on_change : binding.on_result;
            if (binding.closed || generation != binding.generation || current != reference) return 0;
            lua_call(lua, result ? 2 : 1, 0);
            return 0;
        }, 0, 1) < 0) return -1;
        lua_settop(lua, 0);
    }
    return 0;
}

void register_audio(lua_State* lua) {
    luaL_newmetatable(lua, audio_metatable);
    const luaL_Reg audio_methods[] = {
        {"snapshot", guarded<audio_snapshot>}, {"status", guarded<audio_status>},
        {"on_change", guarded<audio_on_change>}, {"on_status", guarded<audio_on_status>},
        {"on_result", guarded<audio_on_result>}, {"refresh", guarded<audio_refresh>}, {"restart", guarded<audio_restart>},
        {"set_volume", guarded<audio_set_volume>}, {"set_mute", guarded<audio_set_mute>},
        {"set_default", guarded<audio_set_default>},
        {"close", guarded<audio_close>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, audio_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<audio_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks audio connection"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<audio_open>); lua_setfield(lua, -2, "audio");
}

}
