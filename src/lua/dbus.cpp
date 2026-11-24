#include "../runtime/invocation.hpp"
#include "../native/dbus_body.hpp"

namespace hyprworks::lua {
namespace {

constexpr char metatable[] = "hyprworks.dbus";
constexpr char fd_metatable[] = "hyprworks.dbus.fd";
using Bus = InvocationState::BusBinding;

dbus::FileDescriptor& descriptor(lua_State* lua, int index) {
    auto* fd = static_cast<dbus::FileDescriptor*>(luaL_testudata(lua, index, fd_metatable));
    if (!fd) throw std::runtime_error("owned D-Bus file descriptor expected");
    return *fd;
}

void push_descriptor(lua_State* lua, const dbus::FileDescriptor& fd) {
    auto* storage = lua_newuserdatauv(lua, sizeof(dbus::FileDescriptor), 0);
    new (storage) dbus::FileDescriptor(fd);
    luaL_setmetatable(lua, fd_metatable);
}

int fd_close(lua_State* lua) { descriptor(lua, 1) = {}; return 0; }
int fd_gc(lua_State* lua) {
    auto& fd = descriptor(lua, 1);
    lua_pushnil(lua); lua_setmetatable(lua, 1);
    std::destroy_at(&fd); return 0;
}
int fd_number(lua_State* lua) { lua_pushinteger(lua, descriptor(lua, 1).get()); return 1; }
int fd_duplicate(lua_State* lua) {
    const auto number = luaL_checkinteger(lua, 1);
    if (number < 0 || number > INT_MAX) return luaL_error(lua, "invalid file descriptor");
    const auto result = dbus::FileDescriptor::duplicate(static_cast<int>(number));
    if (!result) return operation_result(lua, std::expected<void, std::string>(std::unexpected(result.error())));
    return protected_call(lua, [&](lua_State* lua) { push_descriptor(lua, *result); return 1; }, 1);
}

Bus& bus_handle(lua_State* lua, bool closed = false) {
    auto* handle = static_cast<std::shared_ptr<Bus>*>(luaL_testudata(lua, 1, metatable));
    if (!handle) throw std::runtime_error("D-Bus handle expected");
    if (invocation(lua).closing) throw std::runtime_error("invocation is shutting down");
    auto& bus = **handle;
    if (bus.closed && !closed) throw std::runtime_error("D-Bus connection is closed");
    return bus;
}

std::string text(lua_State* lua, int index, std::size_t limit = 16384) {
    if (lua_type(lua, index) != LUA_TSTRING) throw std::runtime_error("expected D-Bus string");
    std::size_t size;
    const char* data = lua_tolstring(lua, index, &size);
    if (size > limit) throw std::length_error("D-Bus string exceeds limit");
    return {data, size};
}

void read_value(lua_State* lua, int index, dbus::Value& value, dbus::BodyBudget& budget, unsigned depth) {
    budget.node(depth);
    if (!lua_checkstack(lua, 8)) throw std::runtime_error("cannot grow Lua stack");
    index = lua_absindex(lua, index);
    const char type = value.signature[0];
    if (type == 's' || type == 'o' || type == 'g' || value.signature == "ay") {
        if (lua_type(lua, index) != LUA_TSTRING) throw std::runtime_error("expected D-Bus string");
        std::size_t size;
        const char* data = lua_tolstring(lua, index, &size);
        budget.payload(size);
        value.data = std::string(data, size);
    } else if (type == 'h') {
        budget.descriptor();
        value.data = descriptor(lua, index);
    } else if (type == 'b') {
        if (lua_type(lua, index) != LUA_TBOOLEAN) throw std::runtime_error("expected D-Bus boolean");
        value.data = lua_toboolean(lua, index) != 0;
    } else if (type == 'd') {
        if (lua_type(lua, index) != LUA_TNUMBER) throw std::runtime_error("expected D-Bus number");
        value.data = static_cast<double>(lua_tonumber(lua, index));
    } else if (std::string_view("ynqiuxt").contains(type)) {
        if (type == 't' && lua_type(lua, index) == LUA_TSTRING) {
            value.data = check_uint64(lua, index);
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
            std::string_view rest = body[0].signature;
            dbus::take_type(rest);
            if (!rest.empty()) throw std::runtime_error("variant needs one complete type");
            field(lua, index, "value");
            read_value(lua, -1, body[0], budget, depth + 1);
            lua_pop(lua, 1);
        } else if (value.signature.starts_with("a{")) {
            const auto dict = std::string_view(value.signature);
            lua_pushnil(lua);
            while (lua_next(lua, index)) {
                budget.node(depth + 1);
                body.push_back({value.signature.substr(1), dbus::Body{{std::string(dict.substr(2, 1)), false},
                    {std::string(dict.substr(3, dict.size() - 4)), false}}});
                auto& pair = std::get<dbus::Body>(body.back().data);
                read_value(lua, -2, pair[0], budget, depth + 2);
                read_value(lua, -1, pair[1], budget, depth + 2);
                lua_pop(lua, 1);
            }
        } else {
            const bool array = type == 'a';
            auto types = std::string_view(value.signature).substr(1, array ? std::string_view::npos : value.signature.size() - 2);
            const auto size = lua_rawlen(lua, index);
            if (size > dbus::BodyBudget::max_nodes - budget.nodes)
                throw std::runtime_error("invalid D-Bus container length");
            for (std::size_t i = 0; i < size; ++i) {
                body.push_back({std::string(array ? types : dbus::take_type(types)), false});
                lua_rawgeti(lua, index, static_cast<lua_Integer>(i + 1));
                read_value(lua, -1, body.back(), budget, depth + 1);
                lua_pop(lua, 1);
            }
            if (!array && !types.empty()) throw std::runtime_error("invalid D-Bus struct length");
        }
    }
}

void read_body(lua_State* lua, int index, std::string_view signature, dbus::Body& body) {
    if (lua_type(lua, index) != LUA_TTABLE) throw std::runtime_error("expected D-Bus arguments");
    dbus::BodyBudget budget;
    index = lua_absindex(lua, index);
    const auto count = lua_rawlen(lua, index);
    for (std::size_t i = 0; i < count; ++i) {
        body.push_back({std::string(dbus::take_type(signature)), false});
        lua_rawgeti(lua, index, static_cast<lua_Integer>(i + 1));
        read_value(lua, -1, body.back(), budget, 0);
        lua_pop(lua, 1);
    }
    if (!signature.empty()) throw std::runtime_error("D-Bus arguments must match signature arity");
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
    else if (const auto* data = std::get_if<dbus::FileDescriptor>(&value.data)) push_descriptor(lua, *data);
    else {
        const auto& body = std::get<dbus::Body>(value.data);
        const bool variant = value.signature == "v", dictionary = value.signature.starts_with("a{");
        lua_createtable(lua, variant || dictionary ? 0 : static_cast<int>(body.size()),
            variant ? 2 : dictionary ? static_cast<int>(body.size()) : 0);
        if (variant) {
            string_field(lua, "signature", body[0].signature);
            push_value(lua, body[0]); lua_setfield(lua, -2, "value");
        } else if (dictionary) {
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
    std::string address;
    bool system = false;
    lua_pushvalue(lua, 1);
    const int parsed = protected_call(lua, [&](lua_State* lua) {
        static const char* choices[] = {"session", "system", nullptr};
        system = enum_field(lua, 2, "bus", nullptr, choices) == 1;
        text_field(lua, 2, "address", address);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    auto& slot = vacant_slot(state.buses);
    auto bus = std::make_shared<Bus>();
    const auto result = bus->connection.start(system, address);
    if (!result) return operation_result(lua, result);
    return open_active(lua, slot, bus, metatable);
}

int bus_call(lua_State* lua) {
    luaL_checktype(lua, 2, LUA_TTABLE);
    dbus::Call call;
    std::string signature;
    lua_pushvalue(lua, 2);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        text_field(lua, 2, "destination", call.destination);
        text_field(lua, 2, "path", call.path);
        text_field(lua, 2, "interface", call.interface);
        text_field(lua, 2, "member", call.member);
        call.timeout = std::chrono::milliseconds(integer_field(lua, 2, "timeout", std::nullopt, 1, 300000));
        call.interactive = boolean_field(lua, 2, "interactive", false);
        text_field(lua, 2, "signature", signature, 255);
        field(lua, 2, "arguments");
        if (!signature.empty() || !lua_isnil(lua, -1)) read_body(lua, -1, signature, call.arguments);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return request_result(lua, bus_handle(lua).connection.call(call));
}

int bus_subscribe(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return request_result(lua, bus.connection.subscribe(text(lua, 2)));
}

int bus_cancel(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.cancel(check_uint64(lua, 2, 1)));
}

int bus_unsubscribe(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.unsubscribe(check_uint64(lua, 2, 1)));
}

int bus_export(lua_State* lua) {
    luaL_checktype(lua, 2, LUA_TTABLE);
    std::string path, interface;
    std::vector<dbus::Method> methods;
    std::vector<dbus::Signal> signals;
    std::vector<dbus::Property> properties;
    std::chrono::milliseconds timeout;
    lua_pushvalue(lua, 2);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        text_field(lua, 2, "path", path); text_field(lua, 2, "interface", interface);
        timeout = std::chrono::milliseconds(integer_field(lua, 2, "timeout", std::nullopt, 1, 300000));
        field(lua, 2, "methods");
        if (!lua_isnil(lua, -1)) luaL_checktype(lua, -1, LUA_TTABLE);
        const auto count = lua_isnil(lua, -1) ? 0 : lua_rawlen(lua, -1);
        if (count > 32) throw std::runtime_error("D-Bus export exceeds 32 methods");
        methods.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); luaL_checktype(lua, -1, LUA_TTABLE);
            text_field(lua, -1, "name", methods[i].name, 255);
            text_field(lua, -1, "input", methods[i].input, 255);
            text_field(lua, -1, "output", methods[i].output, 255);
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);
        field(lua, 2, "signals");
        if (!lua_isnil(lua, -1)) luaL_checktype(lua, -1, LUA_TTABLE);
        const auto signal_count = lua_isnil(lua, -1) ? 0 : lua_rawlen(lua, -1);
        if (signal_count > 32) throw std::runtime_error("D-Bus export exceeds 32 signals");
        signals.resize(signal_count);
        for (std::size_t i = 0; i < signal_count; ++i) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); luaL_checktype(lua, -1, LUA_TTABLE);
            text_field(lua, -1, "name", signals[i].name, 255);
            text_field(lua, -1, "signature", signals[i].signature, 255);
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);
        field(lua, 2, "properties");
        if (!lua_isnil(lua, -1)) luaL_checktype(lua, -1, LUA_TTABLE);
        const auto property_count = lua_isnil(lua, -1) ? 0 : lua_rawlen(lua, -1);
        if (property_count > 32) throw std::runtime_error("D-Bus export exceeds 32 properties");
        properties.resize(property_count);
        dbus::BodyBudget budget;
        for (std::size_t i = 0; i < property_count; ++i) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(i + 1)); luaL_checktype(lua, -1, LUA_TTABLE);
            auto& property = properties[i];
            text_field(lua, -1, "name", property.name, 255);
            text_field(lua, -1, "signature", property.value.signature, 255);
            std::string_view rest = property.value.signature;
            dbus::take_type(rest);
            if (!rest.empty()) throw std::runtime_error("property needs one complete type");
            property.constant = boolean_field(lua, -1, "constant", false);
            field(lua, -1, "value");
            read_value(lua, -1, property.value, budget, 0);
            lua_pop(lua, 2);
        }
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus_handle(lua).connection.export_interface(path, interface, methods, signals, properties, timeout));
}

int bus_request_name(lua_State* lua) {
    return operation_result(lua, bus_handle(lua).connection.request_name(text(lua, 2, 255)));
}

int bus_release_name(lua_State* lua) {
    return operation_result(lua, bus_handle(lua).connection.release_name(text(lua, 2, 255)));
}

int bus_owned_names(lua_State* lua) {
    const auto names = bus_handle(lua).connection.owned_names();
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, static_cast<int>(names.size()), 0);
        lua_Integer index = 1;
        for (const auto& name : names) { lua_pushlstring(lua, name.data(), name.size()); lua_rawseti(lua, -2, index++); }
        return 1;
    }, 1);
}

int bus_unexport(lua_State* lua) {
    return operation_result(lua, bus_handle(lua).connection.unexport(text(lua, 2), text(lua, 3, 255)));
}

int bus_emit(lua_State* lua) {
    luaL_checktype(lua, 2, LUA_TTABLE);
    std::string path, interface, member, destination, signature;
    dbus::Body body;
    lua_pushvalue(lua, 2);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        text_field(lua, 2, "path", path); text_field(lua, 2, "interface", interface);
        text_field(lua, 2, "member", member); text_field(lua, 2, "destination", destination);
        text_field(lua, 2, "signature", signature, 255);
        field(lua, 2, "arguments");
        if (!signature.empty() || !lua_isnil(lua, -1)) read_body(lua, -1, signature, body);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus_handle(lua).connection.emit(path, interface, member, body, destination));
}

int bus_set_property(lua_State* lua) {
    const auto path = text(lua, 2), interface = text(lua, 3, 255), name = text(lua, 4, 255);
    dbus::Value value{text(lua, 5, 255), false};
    std::string_view rest = value.signature;
    dbus::take_type(rest);
    if (!rest.empty()) throw std::runtime_error("property needs one complete type");
    lua_pushvalue(lua, 6);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        dbus::BodyBudget budget;
        read_value(lua, 2, value, budget, 0);
        return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus_handle(lua).connection.set_property(path, interface, name, value));
}

int bus_reply(lua_State* lua) {
    const auto id = check_uint64(lua, 2, 1);
    const auto signature = text(lua, 3, 255);
    dbus::Body body;
    lua_pushvalue(lua, 4);
    const auto parsed = protected_call(lua, [&](lua_State* lua) {
        read_body(lua, 2, signature, body); return 0;
    }, 0, 0, 1);
    if (parsed < 0) return -1;
    return operation_result(lua, bus_handle(lua).connection.reply(id, body));
}

int bus_error(lua_State* lua) {
    auto& bus = bus_handle(lua);
    return operation_result(lua, bus.connection.error(check_uint64(lua, 2, 1), text(lua, 3), text(lua, 4)));
}

int bus_status(lua_State* lua) {
    const auto& bus = bus_handle(lua, true);
    const auto& status = bus.connection.status();
    static const char* phases[] = {"stopped", "connecting", "ready", "failed"};
    return connection_status(lua, bus.closed ? "closed" : phases[static_cast<unsigned>(status.phase)], status.error);
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
    ++bus.generation; bus.status_pending = false;
    bus.connection.stop(); bus.events.clear();
    unref(lua, bus.on_event); unref(lua, bus.on_status);
    untrack(lua, invocation(lua).buses, bus);
    return 0;
}

}

void push_bus_event(lua_State* lua, const dbus::Event& event) {
    static const char* kinds[] = {"reply", "signal", "method", "expired", "name_acquired", "name_lost", "name_failed", "peer_gone"};
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

void register_dbus(lua_State* lua) {
    luaL_newmetatable(lua, fd_metatable);
    const luaL_Reg fd_methods[] = {{"close", guarded<fd_close>}, {"fileno", guarded<fd_number>}, {nullptr, nullptr}};
    userdata_methods(lua, fd_methods);
    lua_pushcfunction(lua, guarded<fd_gc>); lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, guarded<fd_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks owned D-Bus file descriptor"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<fd_duplicate>); lua_setfield(lua, -2, "dbus_fd");
    luaL_newmetatable(lua, metatable);
    const luaL_Reg methods[] = {
        {"call", guarded<bus_call>}, {"cancel", guarded<bus_cancel>},
        {"subscribe", guarded<bus_subscribe>}, {"unsubscribe", guarded<bus_unsubscribe>},
        {"export", guarded<bus_export>}, {"reply", guarded<bus_reply>}, {"error", guarded<bus_error>},
        {"request_name", guarded<bus_request_name>}, {"release_name", guarded<bus_release_name>},
        {"owned_names", guarded<bus_owned_names>}, {"unexport", guarded<bus_unexport>},
        {"emit", guarded<bus_emit>}, {"set_property", guarded<bus_set_property>},
        {"status", guarded<bus_status>}, {"on_status", guarded<bus_on_status>}, {"on_event", guarded<bus_on_event>},
        {"close", guarded<bus_close>}, {nullptr, nullptr},
    };
    userdata_methods(lua, methods);
    lua_pushcfunction(lua, release_userdata<Bus>); lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, guarded<bus_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks D-Bus connection"); lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<bus_open>); lua_setfield(lua, -2, "dbus");
}

}
