#include "../runtime/invocation.hpp"

namespace hyprworks::lua {
namespace {

constexpr char metatable[] = "hyprworks.dbus";
using Bus = InvocationState::BusBinding;

Bus& bus_handle(lua_State* lua, bool closed = false) {
    auto& bus = **static_cast<Bus**>(luaL_checkudata(lua, 1, metatable));
    if (invocation(lua).closing) luaL_error(lua, "invocation is shutting down");
    if (bus.closed && !closed) luaL_error(lua, "D-Bus connection is closed");
    return bus;
}

std::string text(lua_State* lua, int index, std::size_t limit = 16384) {
    if (lua_type(lua, index) != LUA_TSTRING) throw std::runtime_error("expected D-Bus string");
    std::size_t size;
    const char* data = lua_tolstring(lua, index, &size);
    if (size > limit) throw std::length_error("D-Bus string exceeds limit");
    return {data, size};
}

std::uint64_t request_id(lua_State* lua, int index) {
    const auto value = text(lua, index, 20);
    std::uint64_t result;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result)
        throw std::runtime_error("invalid D-Bus request identifier");
    return result;
}

struct Budget {
    std::size_t nodes = 0, bytes = 0;
};

void read_value(lua_State* lua, int index, dbus::Value& value, Budget& budget, unsigned depth) {
    if (depth > 32 || ++budget.nodes > 65536) throw std::length_error("D-Bus value limit exceeded");
    if (!lua_checkstack(lua, 8)) throw std::runtime_error("cannot grow Lua stack");
    index = lua_absindex(lua, index);
    const char type = value.signature[0];
    if (type == 's' || type == 'o' || type == 'g' || value.signature == "ay") {
        value.data = text(lua, index, 8 * 1024 * 1024);
        budget.bytes += std::get<std::string>(value.data).size();
        if (budget.bytes > 8 * 1024 * 1024) throw std::length_error("D-Bus payload exceeds 8 MiB");
    } else if (type == 'b') {
        if (lua_type(lua, index) != LUA_TBOOLEAN) throw std::runtime_error("expected D-Bus boolean");
        value.data = lua_toboolean(lua, index) != 0;
    } else if (type == 'd') {
        if (lua_type(lua, index) != LUA_TNUMBER) throw std::runtime_error("expected D-Bus number");
        value.data = static_cast<double>(lua_tonumber(lua, index));
    } else if (std::string_view("ynqiuxt").contains(type)) {
        if (type == 't' && lua_type(lua, index) == LUA_TSTRING) {
            const auto number = text(lua, index, 20);
            std::uint64_t result;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                throw std::runtime_error("invalid uint64 decimal string");
            value.data = result;
        } else {
            if (!lua_isinteger(lua, index)) throw std::runtime_error("expected D-Bus integer");
            const auto number = lua_tointeger(lua, index);
            if (type == 'n' || type == 'i' || type == 'x') value.data = static_cast<std::int64_t>(number);
            else {
                if (number < 0) throw std::runtime_error("negative unsigned D-Bus integer");
                value.data = static_cast<std::uint64_t>(number);
            }
        }
    } else {
        if (lua_type(lua, index) != LUA_TTABLE) throw std::runtime_error("expected D-Bus container table");
        value.data = dbus::Body{};
        auto& body = std::get<dbus::Body>(value.data);
        if (type == 'v') {
            field(lua, index, "signature");
            body.push_back({text(lua, -1, 255), false});
            lua_pop(lua, 1);
            if (dbus::split_signature(body[0].signature).size() != 1) throw std::runtime_error("variant needs one complete type");
            field(lua, index, "value");
            read_value(lua, -1, body[0], budget, depth + 1);
            lua_pop(lua, 1);
        } else if (value.signature.starts_with("a{")) {
            const auto types = dbus::split_signature(value.signature.substr(2, value.signature.size() - 3));
            lua_pushnil(lua);
            while (lua_next(lua, index)) {
                body.push_back({value.signature.substr(1), dbus::Body{{types[0], false}, {types[1], false}}});
                auto& pair = std::get<dbus::Body>(body.back().data);
                read_value(lua, -2, pair[0], budget, depth + 1);
                read_value(lua, -1, pair[1], budget, depth + 1);
                lua_pop(lua, 1);
            }
        } else {
            const auto types = dbus::split_signature(type == 'a' ? value.signature.substr(1) :
                value.signature.substr(1, value.signature.size() - 2));
            const auto size = lua_rawlen(lua, index);
            if (size > 65536 || (type != 'a' && size != types.size())) throw std::runtime_error("invalid D-Bus container length");
            body.reserve(size);
            for (std::size_t i = 0; i < size; ++i) {
                body.push_back({types[type == 'a' ? 0 : i], false});
                lua_rawgeti(lua, index, static_cast<lua_Integer>(i + 1));
                read_value(lua, -1, body.back(), budget, depth + 1);
                lua_pop(lua, 1);
            }
        }
    }
}

void read_body(lua_State* lua, int index, const std::string& signature, dbus::Body& body) {
    const auto types = dbus::split_signature(signature);
    if (lua_type(lua, index) != LUA_TTABLE || lua_rawlen(lua, index) != types.size())
        throw std::runtime_error("D-Bus arguments must match signature arity");
    Budget budget;
    index = lua_absindex(lua, index);
    for (std::size_t i = 0; i < types.size(); ++i) {
        body.push_back({types[i], false});
        lua_rawgeti(lua, index, static_cast<lua_Integer>(i + 1));
        read_value(lua, -1, body.back(), budget, 0);
        lua_pop(lua, 1);
    }
}

void push_value(lua_State* lua, const dbus::Value& value) {
    if (!lua_checkstack(lua, 8)) throw std::runtime_error("cannot grow Lua stack");
    if (const auto* data = std::get_if<bool>(&value.data)) lua_pushboolean(lua, *data);
    else if (const auto* data = std::get_if<std::int64_t>(&value.data)) lua_pushinteger(lua, *data);
    else if (const auto* data = std::get_if<std::uint64_t>(&value.data)) {
        if (*data > static_cast<std::uint64_t>(LUA_MAXINTEGER)) identifier(lua, *data);
        else lua_pushinteger(lua, static_cast<lua_Integer>(*data));
    } else if (const auto* data = std::get_if<double>(&value.data)) lua_pushnumber(lua, *data);
    else if (const auto* data = std::get_if<std::string>(&value.data)) lua_pushlstring(lua, data->data(), data->size());
    else {
        const auto& body = std::get<dbus::Body>(value.data);
        lua_createtable(lua, value.signature.starts_with("a{") ? 0 : static_cast<int>(body.size()), 0);
        if (value.signature == "v") {
            string_field(lua, "signature", body[0].signature);
            push_value(lua, body[0]); lua_setfield(lua, -2, "value");
        } else if (value.signature.starts_with("a{")) {
            for (const auto& entry : body) {
                const auto& pair = std::get<dbus::Body>(entry.data);
                push_value(lua, pair[0]); push_value(lua, pair[1]); lua_rawset(lua, -3);
            }
        } else {
            lua_Integer index = 1;
            for (const auto& item : body) { push_value(lua, item); lua_rawseti(lua, -2, index++); }
        }
    }
}

int bus_open(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) return luaL_error(lua, "invocation is shutting down");
    luaL_checktype(lua, 1, LUA_TTABLE);
    if (state.buses.size() >= 8) return luaL_error(lua, "D-Bus handle limit reached");
    auto bus = std::make_unique<Bus>();
    std::string address;
    bool system = false;
    lua_pushvalue(lua, 1);
    const int parsed = protected_call(lua, [&](lua_State* lua) {
        static const char* choices[] = {"session", "system", nullptr};
        system = enum_field(lua, 2, "bus", "session", choices) == 1;
        text_field(lua, 2, "address", address);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    const auto result = bus->connection.start(system, address);
    if (!result) return operation_result(lua, result);
    auto& binding = *bus;
    state.buses.push_back(std::move(bus));
    const int status = retain_handle(lua, binding, metatable);
    if (status < 0) { binding.closed = true; binding.connection.stop(); }
    return status;
}

int bus_call(lua_State* lua) {
    auto& bus = bus_handle(lua);
    luaL_checktype(lua, 2, LUA_TTABLE);
    dbus::Call call;
    std::string signature;
    lua_pushvalue(lua, 2);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        text_field(lua, 2, "destination", call.destination);
        text_field(lua, 2, "path", call.path);
        text_field(lua, 2, "interface", call.interface);
        text_field(lua, 2, "member", call.member);
        call.timeout = std::chrono::milliseconds(integer_field(lua, 2, "timeout", 5000, 1, 300000));
        call.interactive = boolean_field(lua, 2, "interactive", false);
        text_field(lua, 2, "signature", signature, 255);
        field(lua, 2, "arguments");
        if (!signature.empty() || !lua_isnil(lua, -1)) read_body(lua, -1, signature, call.arguments);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return request_result(lua, bus.connection.call(call));
}

int bus_subscribe(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return request_result(lua, bus.connection.subscribe(text(lua, 2)));
}

int bus_cancel(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.cancel(request_id(lua, 2)));
}

int bus_unsubscribe(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.unsubscribe(request_id(lua, 2)));
}

int bus_export(lua_State* lua) {
    auto& bus = bus_handle(lua);
    luaL_checktype(lua, 2, LUA_TTABLE);
    std::string path, interface;
    std::vector<dbus::Method> methods;
    std::chrono::milliseconds timeout;
    lua_pushvalue(lua, 2);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        text_field(lua, 2, "path", path); text_field(lua, 2, "interface", interface);
        timeout = std::chrono::milliseconds(integer_field(lua, 2, "timeout", 120000, 1, 300000));
        field(lua, 2, "methods"); luaL_checktype(lua, -1, LUA_TTABLE);
        const auto count = lua_rawlen(lua, -1);
        if (count == 0 || count > 32) throw std::runtime_error("D-Bus export needs 1..32 methods");
        methods.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); luaL_checktype(lua, -1, LUA_TTABLE);
            text_field(lua, -1, "name", methods[i].name, 255);
            text_field(lua, -1, "input", methods[i].input, 255);
            text_field(lua, -1, "output", methods[i].output, 255);
            lua_pop(lua, 1);
        }
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus.connection.export_methods(path, interface, methods, timeout));
}

int bus_reply(lua_State* lua) {
    auto& bus = bus_handle(lua);
    const auto id = request_id(lua, 2);
    const auto signature = text(lua, 3, 255);
    dbus::Body body;
    lua_pushvalue(lua, 4);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        read_body(lua, 2, signature, body); return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus.connection.reply(id, body));
}

int bus_error(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.error(request_id(lua, 2), text(lua, 3), text(lua, 4)));
}

int bus_status(lua_State* lua) {
    const auto& bus = bus_handle(lua, true);
    const auto status = bus.connection.status();
    const bool closed = bus.closed;
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"stopped", "connecting", "ready", "failed"};
        lua_createtable(lua, 0, 2);
        lua_pushstring(lua, closed ? "closed" : phases[static_cast<unsigned>(status.phase)]);
        lua_setfield(lua, -2, "phase");
        if (!status.error.empty()) string_field(lua, "error", status.error);
        return 1;
    }, 1);
}

int bus_on_event(lua_State* lua) {
    auto& bus = bus_handle(lua);
    replace_callback(lua, 2, bus.on_event, bus.closed);
    return 0;
}

int bus_on_status(lua_State* lua) {
    auto& bus = bus_handle(lua);
    bus.status_pending = replace_callback(lua, 2, bus.on_status, bus.closed);
    return 0;
}

int bus_close(lua_State* lua) {
    auto& bus = bus_handle(lua, true);
    bus.closed = true; ++bus.generation; bus.status_pending = false;
    bus.connection.stop(); bus.events.clear();
    unref(lua, bus.on_event); unref(lua, bus.on_status);
    return 0;
}

}

int dispatch_dbus(lua_State* lua, InvocationState::BusBinding& bus) {
    const auto generation = bus.generation;
    const auto events = std::exchange(bus.events, {});
    const bool status = std::exchange(bus.status_pending, false);
    for (std::size_t i = 0; i <= events.size(); ++i) {
        if (i == 0 && !status) continue;
        const int& callback = i == 0 ? bus.on_status : bus.on_event;
        if (dispatch_callback(lua, bus, callback, generation, [&](lua_State* lua) {
            if (i > 0) {
                const auto& event = events[i - 1];
                static const char* kinds[] = {"reply", "signal", "method", "expired"};
                lua_createtable(lua, 0, 9);
                lua_pushstring(lua, kinds[static_cast<unsigned>(event.kind)]); lua_setfield(lua, -2, "kind");
                identifier(lua, event.id); lua_setfield(lua, -2, "id");
                string_field(lua, "sender", event.sender); string_field(lua, "path", event.path);
                string_field(lua, "interface", event.interface); string_field(lua, "member", event.member);
                if (!event.error.empty()) { string_field(lua, "error", event.error); string_field(lua, "error_name", event.error_name); }
                lua_createtable(lua, static_cast<int>(event.arguments.size()), 0);
                lua_Integer index = 1;
                for (const auto& value : event.arguments) { push_value(lua, value); lua_rawseti(lua, -2, index++); }
                lua_setfield(lua, -2, "arguments");
            }
            return i ? 1 : 0;
        }) < 0) return -1;
    }
    return 0;
}

void register_dbus(lua_State* lua) {
    luaL_newmetatable(lua, metatable);
    const luaL_Reg methods[] = {
        {"call", guarded<bus_call>}, {"cancel", guarded<bus_cancel>},
        {"subscribe", guarded<bus_subscribe>}, {"unsubscribe", guarded<bus_unsubscribe>},
        {"export", guarded<bus_export>}, {"reply", guarded<bus_reply>}, {"error", guarded<bus_error>},
        {"status", guarded<bus_status>}, {"on_status", guarded<bus_on_status>}, {"on_event", guarded<bus_on_event>},
        {"close", guarded<bus_close>}, {nullptr, nullptr},
    };
    luaL_setfuncs(lua, methods, 0);
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<bus_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks D-Bus connection"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<bus_open>); lua_setfield(lua, -2, "dbus");
}

}
