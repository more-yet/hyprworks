#include "../runtime/invocation.hpp"
#include <climits>

namespace hyprworks::lua {
namespace {

constexpr char polkit_type[] = "hyprworks.polkit";

PolkitBinding& handle(lua_State* lua, bool allow_closed = false) {
    auto& binding = **static_cast<PolkitBinding**>(luaL_checkudata(lua, 1, polkit_type));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (binding.closed && !allow_closed) luaL_error(lua, "Polkit agent is closed");
    return binding;
}

int open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    if (!state.polkit) {
        polkit::Options options;
        if (lua_isnone(lua, 1)) lua_pushnil(lua); else lua_pushvalue(lua, 1);
        if (protected_call(lua, [&](lua_State* lua) {
            if (lua_isnil(lua, 2)) return 0;
            luaL_checktype(lua, 2, LUA_TTABLE);
            text_field(lua, 2, "session", options.session, 256);
            text_field(lua, 2, "locale", options.locale, 256);
            return 0;
        }, 0, 0, 1) < 0) return -1;
        if (state.polkit) throw std::runtime_error("Polkit agent opened while parsing options");
        auto& binding = state.polkit.emplace();
        binding.options = std::move(options);
        const auto result = binding.agent.start(binding.options);
        if (!result) {
            state.polkit.reset();
            return operation_result(lua, result);
        }
    } else if (!lua_isnoneornil(lua, 1)) return luaL_error(lua, "Polkit agent already exists; use hw.polkit() to share it");
    auto& binding = *state.polkit;
    const int result = retain_handle(lua, binding, polkit_type);
    if (result < 0) { binding.closed = true; binding.agent.stop(); }
    return result;
}

int status(lua_State* lua) {
    const auto& binding = handle(lua, true);
    const auto& status = binding.agent.status();
    static const char* phases[] = {"stopped", "connecting", "ready", "failed"};
    return connection_status(lua, binding.closed ? "closed" : phases[static_cast<unsigned>(status.phase)], status.error);
}

int on_status(lua_State* lua) {
    auto& binding = handle(lua);
    binding.status_pending = replace_callback(lua, 2, binding.on_status, binding.closed);
    return 0;
}
int on_event(lua_State* lua) {
    auto& binding = handle(lua);
    replace_callback(lua, 2, binding.on_event, binding.closed);
    return 0;
}
int restart(lua_State* lua) {
    auto& binding = handle(lua, true);
    const auto result = binding.agent.start(binding.options);
    ++binding.generation; binding.closed = false; binding.events.clear(); binding.status_pending = true;
    return operation_result(lua, result);
}
int close(lua_State* lua) {
    auto& binding = handle(lua, true);
    binding.agent.stop(); binding.closed = true; ++binding.generation;
    binding.events.clear(); binding.status_pending = false;
    unref(lua, binding.on_event); unref(lua, binding.on_status);
    return 0;
}
int authenticate(lua_State* lua) {
    const auto request = check_uint64(lua, 2, 1);
    const auto uid = check_integer(lua, 3);
    luaL_argcheck(lua, uid >= 0 && uid <= INT_MAX, 3, "Unix uid is out of range");
    return operation_result(lua, handle(lua).agent.authenticate(request, static_cast<std::uint32_t>(uid)));
}
int respond(lua_State* lua) {
    const auto request = check_uint64(lua, 2, 1), prompt = check_uint64(lua, 3, 1);
    std::size_t size;
    const char* response = check_string(lua, 4, &size);
    return operation_result(lua, handle(lua).agent.respond(request, prompt, {response, size}));
}
int cancel(lua_State* lua) {
    const auto request = check_uint64(lua, 2, 1);
    return operation_result(lua, handle(lua).agent.cancel(request));
}

}

int dispatch_polkit(lua_State* lua, PolkitBinding& binding) {
    const auto events = std::exchange(binding.events, {});
    const auto generation = binding.generation;
    if (std::exchange(binding.status_pending, false) &&
        dispatch_callback(lua, binding, binding.on_status, generation, [](lua_State*) { return 0; }) < 0) return -1;
    for (const auto& event : events) {
        const auto current = [&] {
            return !binding.closed && binding.generation == generation &&
                !invocation(lua).requests.load(std::memory_order_relaxed) &&
                (event.kind == polkit::EventKind::completed || binding.agent.pending(event.id, event.prompt));
        };
        if (!current()) continue;
        const int callback = binding.on_event;
        if (callback < 0) {
            // Native cancellation failures are delivered through agent status.
            if (event.kind == polkit::EventKind::request) (void)binding.agent.cancel(event.id);
            continue;
        }
        lua_pushcfunction(lua, traceback);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, callback);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, binding.handle);
            lua_createtable(lua, 0, event.request ? 7 : 5);
            static const char* kinds[] = {"request", "prompt", "info", "error", "completed"};
            lua_pushstring(lua, kinds[static_cast<unsigned>(event.kind)]); lua_setfield(lua, -2, "kind");
            identifier(lua, event.id); lua_setfield(lua, -2, "id");
            if (event.request) {
                const auto& request = *event.request;
                string_field(lua, "action", request.action); string_field(lua, "message", request.message);
                string_field(lua, "icon", request.icon);
                lua_createtable(lua, 0, static_cast<int>(request.details.size()));
                for (const auto& [key, value] : request.details) {
                    lua_pushlstring(lua, key.data(), key.size());
                    lua_pushlstring(lua, value.data(), value.size()); lua_rawset(lua, -3);
                }
                lua_setfield(lua, -2, "details");
                lua_createtable(lua, static_cast<int>(request.identities.size()), 0);
                lua_Integer index = 1;
                for (const auto& identity : request.identities) {
                    lua_createtable(lua, 0, 2);
                    lua_pushinteger(lua, identity.uid); lua_setfield(lua, -2, "uid");
                    string_field(lua, "name", identity.name); lua_rawseti(lua, -2, index++);
                }
                lua_setfield(lua, -2, "identities");
            } else if (event.kind == polkit::EventKind::completed) {
                lua_pushboolean(lua, event.authorized); lua_setfield(lua, -2, "authorized");
                lua_pushboolean(lua, event.cancelled); lua_setfield(lua, -2, "cancelled");
            } else {
                string_field(lua, "text", event.text);
                if (event.kind == polkit::EventKind::prompt) {
                    identifier(lua, event.prompt); lua_setfield(lua, -2, "prompt");
                    lua_pushboolean(lua, event.echo); lua_setfield(lua, -2, "echo");
                }
            }
            if (!current() || callback != binding.on_event) return 0;
            lua_call(lua, 2, 0);
            return 0;
        }, 0, 1) < 0) return -1;
        lua_settop(lua, 0);
    }
    return 0;
}

void register_polkit(lua_State* lua) {
    luaL_newmetatable(lua, polkit_type);
    const luaL_Reg methods[] = {{"status", guarded<status>}, {"on_status", guarded<on_status>},
        {"on_event", guarded<on_event>}, {"authenticate", guarded<authenticate>}, {"respond", guarded<respond>},
        {"cancel", guarded<cancel>}, {"restart", guarded<restart>}, {"close", guarded<close>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, methods, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks Polkit agent"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<open>); lua_setfield(lua, -2, "polkit");
}

}
