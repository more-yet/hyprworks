#include "invocation.hpp"

#include <utility>

namespace hyprworks::lua {
namespace {

constexpr char media_metatable[] = "hyprworks.mpris";

MediaBinding& media_handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<MediaBinding**>(luaL_checkudata(lua, 1, media_metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "MPRIS connection is closed");
    return binding;
}

int media_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.media) {
        state.media.emplace();
        const auto started = state.media->connection.start();
        state.media->status_pending = !started;
    }
    auto& binding = *state.media;
    if (binding.opening) return luaL_error(lua, "MPRIS handle is being opened");
    const int status = protected_call(lua, [&](lua_State* lua) {
        if (binding.handle >= 0) { lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle); return 1; }
        binding.opening = true;
        *static_cast<MediaBinding**>(lua_newuserdatauv(lua, sizeof(MediaBinding*), 0)) = &binding;
        luaL_setmetatable(lua, media_metatable);
        lua_pushvalue(lua, -1);
        binding.handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }, 1);
    binding.opening = false;
    return status;
}

void push_strings(lua_State* lua, const std::vector<std::string>& values) {
    lua_createtable(lua, static_cast<int>(values.size()), 0);
    lua_Integer index = 1;
    for (const auto& value : values) {
        lua_pushlstring(lua, value.data(), value.size());
        lua_rawseti(lua, -2, index++);
    }
}

int media_snapshot(lua_State* lua) {
    const auto snapshot = media_handle(lua).connection.snapshot();
    const auto now = mpris::Clock::now();
    return protected_call(lua, [&](lua_State* lua) {
        if (!snapshot) { lua_pushnil(lua); return 1; }
        lua_createtable(lua, 0, 2);
        identifier(lua, snapshot->revision); lua_setfield(lua, -2, "revision");
        lua_createtable(lua, static_cast<int>(snapshot->players.size()), 0);
        lua_Integer index = 1;
        for (const auto& player : snapshot->players) {
            lua_createtable(lua, 0, 25);
            string_field(lua, "name", player.key.name); string_field(lua, "owner", player.key.owner);
            string_field(lua, "identity", player.identity); string_field(lua, "desktop_entry", player.desktop_entry);
            string_field(lua, "playback_status", player.playback_status);
            if (!player.error.empty()) string_field(lua, "error", player.error);
            for (const auto& [name, value] : {std::pair{"available", player.available}, {"can_control", player.can_control},
                    {"can_play", player.can_play}, {"can_pause", player.can_pause}, {"can_seek", player.can_seek},
                    {"can_go_next", player.can_go_next}, {"can_go_previous", player.can_go_previous}, {"can_raise", player.can_raise}, {"can_quit", player.can_quit}}) {
                lua_pushboolean(lua, value); lua_setfield(lua, -2, name);
            }
            for (const auto& [name, value] : {std::pair{"rate", player.rate}, {"minimum_rate", player.minimum_rate}, {"maximum_rate", player.maximum_rate}}) {
                lua_pushnumber(lua, value); lua_setfield(lua, -2, name);
            }
            if (player.volume) { lua_pushnumber(lua, *player.volume); lua_setfield(lua, -2, "volume"); }
            if (player.shuffle) { lua_pushboolean(lua, *player.shuffle); lua_setfield(lua, -2, "shuffle"); }
            if (player.loop_status) string_field(lua, "loop_status", *player.loop_status);
            if (const auto position = mpris::estimated_position(player, now)) { lua_pushinteger(lua, *position); lua_setfield(lua, -2, "position"); }
            push_strings(lua, player.uri_schemes); lua_setfield(lua, -2, "uri_schemes");
            push_strings(lua, player.mime_types); lua_setfield(lua, -2, "mime_types");
            lua_createtable(lua, 0, 9);
            string_field(lua, "track_id", player.metadata.track_id); string_field(lua, "title", player.metadata.title);
            string_field(lua, "album", player.metadata.album); string_field(lua, "artwork_url", player.metadata.artwork_url);
            string_field(lua, "url", player.metadata.url);
            push_strings(lua, player.metadata.artists); lua_setfield(lua, -2, "artists");
            push_strings(lua, player.metadata.album_artists); lua_setfield(lua, -2, "album_artists");
            push_strings(lua, player.metadata.genres); lua_setfield(lua, -2, "genres");
            if (player.metadata.length) { lua_pushinteger(lua, *player.metadata.length); lua_setfield(lua, -2, "length"); }
            lua_setfield(lua, -2, "metadata");
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "players");
        return 1;
    }, 1);
}

int media_status(lua_State* lua) {
    const auto& binding = media_handle(lua, true);
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

int media_on_status(lua_State* lua) {
    auto& binding = media_handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}

int media_on_change(lua_State* lua) {
    auto& binding = media_handle(lua);
    binding.change_pending = replace_callback(lua, 2, binding.on_change, binding.closed);
    return 0;
}

int media_on_result(lua_State* lua) {
    auto& binding = media_handle(lua);
    replace_callback(lua, 2, binding.on_result, binding.closed);
    return 0;
}

void push_media_result(lua_State* lua, const mpris::RequestResult& result) {
    static const char* outcomes[] = {"acknowledged", "rejected", "failed", "cancelled"};
    lua_createtable(lua, 0, 4);
    identifier(lua, result.id); lua_setfield(lua, -2, "id");
    lua_pushstring(lua, outcomes[static_cast<unsigned>(result.outcome)]); lua_setfield(lua, -2, "outcome");
    lua_pushboolean(lua, result.may_have_executed); lua_setfield(lua, -2, "may_have_executed");
    if (!result.error.empty()) string_field(lua, "error", result.error);
}

int media_refresh(lua_State* lua) {
    auto& binding = media_handle(lua);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const auto result = binding.connection.refresh();
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushboolean(lua, true); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_restart(lua_State* lua) {
    auto& binding = media_handle(lua, true);
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const auto before = binding.connection.status();
    const bool had_snapshot = binding.connection.snapshot().has_value();
    const auto result = binding.connection.start();
    binding.closed = false;
    if (result || before != binding.connection.status()) { ++binding.generation; binding.status_pending = true; }
    binding.change_pending |= had_snapshot && !binding.connection.snapshot();
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { lua_pushboolean(lua, true); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_action(lua_State* lua) {
    auto& binding = media_handle(lua);
    static const char* actions[] = {"play", "pause", "play_pause", "stop", "next", "previous", "raise", "quit", nullptr};
    const auto action = luaL_checkoption(lua, 3, nullptr, actions);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.action(key, static_cast<mpris::Action>(action));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_seek(lua_State* lua) {
    auto& binding = media_handle(lua);
    const auto offset = luaL_checkinteger(lua, 3);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.seek(key, offset);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_set_position(lua_State* lua) {
    auto& binding = media_handle(lua);
    std::size_t track_length;
    const char* track = luaL_checklstring(lua, 3, &track_length);
    luaL_argcheck(lua, track_length <= 16384, 3, "text exceeds 16 KiB");
    const auto position = luaL_checkinteger(lua, 4);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.set_position(key, std::string(track, track_length), position);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_set_volume(lua_State* lua) {
    auto& binding = media_handle(lua);
    const auto volume = luaL_checknumber(lua, 3);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.set_volume(key, volume);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_set_rate(lua_State* lua) {
    auto& binding = media_handle(lua);
    const auto rate = luaL_checknumber(lua, 3);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.set_rate(key, rate);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_set_shuffle(lua_State* lua) {
    auto& binding = media_handle(lua);
    luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool shuffle = lua_toboolean(lua, 3);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.set_shuffle(key, shuffle);
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_set_loop_status(lua_State* lua) {
    auto& binding = media_handle(lua);
    std::size_t status_length;
    const char* status = luaL_checklstring(lua, 3, &status_length);
    luaL_argcheck(lua, status_length <= 16384, 3, "text exceeds 16 KiB");
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.set_loop_status(key, std::string(status, status_length));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_open_uri(lua_State* lua) {
    auto& binding = media_handle(lua);
    std::size_t uri_length;
    const char* uri = luaL_checklstring(lua, 3, &uri_length);
    luaL_argcheck(lua, uri_length <= 16384, 3, "text exceeds 16 KiB");
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "name");
    std::size_t name_length, owner_length;
    const char* name = luaL_checklstring(lua, -1, &name_length);
    lua_getfield(lua, 2, "owner");
    const char* owner = luaL_checklstring(lua, -1, &owner_length);
    luaL_argcheck(lua, name_length <= 255 && owner_length <= 255, 2, "invalid player key");
    if (binding.closed) return luaL_error(lua, "MPRIS connection closed while parsing request");
    if (binding.result) { lua_pushnil(lua); lua_pushliteral(lua, "an MPRIS result awaits delivery"); return 2; }
    const mpris::PlayerKey key{std::string(name, name_length), std::string(owner, owner_length)};
    const auto result = binding.connection.open_uri(key, std::string(uri, uri_length));
    return protected_call(lua, [&](lua_State* lua) {
        if (result) { identifier(lua, *result); return 1; }
        lua_pushnil(lua); lua_pushlstring(lua, result.error().data(), result.error().size()); return 2;
    }, 2);
}

int media_close(lua_State* lua) {
    auto& binding = media_handle(lua, true);
    auto result = binding.connection.stop();
    if (binding.result) result = std::exchange(binding.result, {});
    binding.closed = true; ++binding.generation;
    binding.change_pending = binding.status_pending = false;
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_change, LUA_NOREF));
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_status, LUA_NOREF));
    luaL_unref(lua, LUA_REGISTRYINDEX, std::exchange(binding.on_result, LUA_NOREF));
    return protected_call(lua, [&](lua_State* lua) { if (result) push_media_result(lua, *result); else lua_pushnil(lua); return 1; }, 1);
}

}

int dispatch_media(lua_State* lua, MediaBinding& binding) {
    const bool status = std::exchange(binding.status_pending, false), change = std::exchange(binding.change_pending, false);
    const auto generation = binding.generation;
    for (const auto kind : {Event::status, Event::change, Event::result}) {
        if (binding.closed || generation != binding.generation || invocation(lua).quit_requested.load(std::memory_order_relaxed)) break;
        if (!(kind == Event::status ? status : kind == Event::change ? change : binding.result.has_value())) continue;
        auto result = kind == Event::result ? std::exchange(binding.result, {}) : std::nullopt;
        const int reference = kind == Event::status ? binding.on_status : kind == Event::change ? binding.on_change : binding.on_result;
        if (reference < 0) continue;
        lua_pushcfunction(lua, traceback);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, reference); lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
            if (result) push_media_result(lua, *result);
            const int current = kind == Event::status ? binding.on_status : kind == Event::change ? binding.on_change : binding.on_result;
            if (binding.closed || generation != binding.generation || current != reference) return 0;
            lua_call(lua, result ? 2 : 1, 0); return 0;
        }, 0, 1) < 0) return -1;
        lua_settop(lua, 0);
    }
    return 0;
}

void register_mpris(lua_State* lua) {
    luaL_newmetatable(lua, media_metatable);
    const luaL_Reg media_methods[] = {
        {"snapshot", guarded<media_snapshot>}, {"status", guarded<media_status>},
        {"on_change", guarded<media_on_change>}, {"on_status", guarded<media_on_status>}, {"on_result", guarded<media_on_result>},
        {"refresh", guarded<media_refresh>}, {"restart", guarded<media_restart>}, {"close", guarded<media_close>},
        {"action", guarded<media_action>}, {"seek", guarded<media_seek>},
        {"set_position", guarded<media_set_position>}, {"set_volume", guarded<media_set_volume>},
        {"set_rate", guarded<media_set_rate>}, {"set_shuffle", guarded<media_set_shuffle>},
        {"set_loop_status", guarded<media_set_loop_status>}, {"open_uri", guarded<media_open_uri>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, media_methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<media_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks MPRIS connection"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<media_open>); lua_setfield(lua, -2, "mpris");
}

}
