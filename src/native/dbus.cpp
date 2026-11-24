#include <hyprworks/dbus.hpp>
#include "dbus.hpp"
#include "dbus_body.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>
#include <fcntl.h>

namespace hyprworks::dbus {
namespace {

std::size_t type_end(std::string_view signature, std::size_t at, unsigned depth, bool dictionary) {
    if (depth > 32 || at >= signature.size()) throw std::runtime_error("invalid D-Bus signature");
    const char type = signature[at++];
    if (std::string_view("ybnqiuxtdsoghv").contains(type)) return at;
    if (type == 'a') return type_end(signature, at, depth + 1, true);
    if (type == '(') {
        const auto begin = at;
        while (at < signature.size() && signature[at] != ')') at = type_end(signature, at, depth + 1, false);
        if (at == begin || at == signature.size()) throw std::runtime_error("invalid D-Bus struct signature");
        return at + 1;
    }
    if (type == '{' && dictionary) {
        if (at == signature.size() || !std::string_view("ybnqiuxtdsogh").contains(signature[at++]))
            throw std::runtime_error("invalid D-Bus dictionary key");
        at = type_end(signature, at, depth + 1, false);
        if (at == signature.size() || signature[at] != '}') throw std::runtime_error("invalid D-Bus dictionary signature");
        return at + 1;
    }
    throw std::runtime_error("unsupported D-Bus type");
}

void validate_value_signature(std::string_view signature) {
    if (signature.size() > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
    if (type_end(signature, 0, 0, false) != signature.size())
        throw std::runtime_error("D-Bus value requires one complete type");
}

Value read_value(sd_bus_message* message, BodyBudget& budget, unsigned depth) {
    budget.node(depth);
    char type;
    const char* contents = nullptr;
    require(sd_bus_message_peek_type(message, &type, &contents));
    Value value;
    value.signature = std::string(1, type);
    switch (type) {
        case 'b': value.data = boolean(message); break;
        case 'y': { std::uint8_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 'q': { std::uint16_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 'u': { std::uint32_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 't': { std::uint64_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = data; break; }
        case 'n': { std::int16_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::int64_t(data); break; }
        case 'i': { std::int32_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::int64_t(data); break; }
        case 'x': { std::int64_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = data; break; }
        case 'd': value.data = finite_number(message); break;
        case 'h': {
            budget.descriptor();
            int fd; require(sd_bus_message_read_basic(message, type, &fd));
            auto owned = FileDescriptor::duplicate(fd);
            if (!owned) throw std::runtime_error(owned.error());
            value.data = std::move(*owned);
            break;
        }
        case 's': case 'o': case 'g': {
            const char* data; require(sd_bus_message_read_basic(message, type, &data));
            const auto size = std::strlen(data); budget.payload(size);
            value.data = std::string(data, size); break;
        }
        case 'a': case 'r': case 'e': case 'v': {
            if (type == 'a') value.signature += contents;
            else if (type == 'r') value.signature = "(" + std::string(contents) + ")";
            else if (type == 'e') value.signature = "{" + std::string(contents) + "}";
            if (value.signature == "ay") {
                const void* data; std::size_t size;
                require(sd_bus_message_read_array(message, 'y', &data, &size));
                budget.payload(size);
                value.data = size ? std::string(static_cast<const char*>(data), size) : std::string{};
                break;
            }
            require(sd_bus_message_enter_container(message, type, contents));
            Body body;
            while (!check(sd_bus_message_at_end(message, false))) body.push_back(read_value(message, budget, depth + 1));
            check(sd_bus_message_exit_container(message));
            value.data = std::move(body);
            break;
        }
        default: throw std::runtime_error("unsupported D-Bus type");
    }
    return value;
}

void write_value(sd_bus_message* message, const Value& value, BodyBudget& budget, unsigned depth) {
    budget.node(depth);
    // Root and variant signatures are validated; other children match their parent's type.
    const char type = value.signature[0];
    switch (type) {
        case 'h': {
            budget.descriptor();
            const int fd = std::get<FileDescriptor>(value.data).get();
            if (fd < 0) throw std::runtime_error("closed D-Bus file descriptor");
            check(sd_bus_message_append_basic(message, type, &fd));
            break;
        }
        case 'b': { const int data = std::get<bool>(value.data); check(sd_bus_message_append_basic(message, type, &data)); break; }
        case 'y': case 'q': case 'u': case 't': {
            const auto data = std::get<std::uint64_t>(value.data);
            const auto limit = type == 'y' ? UINT8_MAX : type == 'q' ? UINT16_MAX : type == 'u' ? UINT32_MAX : UINT64_MAX;
            if (data > limit) throw std::runtime_error("D-Bus unsigned integer out of range");
            if (type == 'y') { const auto number = static_cast<std::uint8_t>(data); check(sd_bus_message_append_basic(message, type, &number)); }
            else if (type == 'q') { const auto number = static_cast<std::uint16_t>(data); check(sd_bus_message_append_basic(message, type, &number)); }
            else if (type == 'u') { const auto number = static_cast<std::uint32_t>(data); check(sd_bus_message_append_basic(message, type, &number)); }
            else check(sd_bus_message_append_basic(message, type, &data));
            break;
        }
        case 'n': case 'i': case 'x': {
            const auto data = std::get<std::int64_t>(value.data);
            if ((type == 'n' && (data < INT16_MIN || data > INT16_MAX)) || (type == 'i' && (data < INT32_MIN || data > INT32_MAX)))
                throw std::runtime_error("D-Bus signed integer out of range");
            if (type == 'n') { const auto number = static_cast<std::int16_t>(data); check(sd_bus_message_append_basic(message, type, &number)); }
            else if (type == 'i') { const auto number = static_cast<std::int32_t>(data); check(sd_bus_message_append_basic(message, type, &number)); }
            else check(sd_bus_message_append_basic(message, type, &data));
            break;
        }
        case 'd': {
            const auto data = std::get<double>(value.data);
            if (!std::isfinite(data)) throw std::runtime_error("nonfinite D-Bus number");
            check(sd_bus_message_append_basic(message, type, &data)); break;
        }
        case 's': case 'o': case 'g': {
            const auto& data = std::get<std::string>(value.data);
            budget.payload(data.size());
            if (data.contains('\0')) throw std::runtime_error("D-Bus text contains NUL");
            check(sd_bus_message_append_basic(message, type, data.c_str())); break;
        }
        default: {
            if (value.signature == "ay") {
                const auto& data = std::get<std::string>(value.data);
                budget.payload(data.size());
                check(sd_bus_message_append_array(message, 'y', data.data(), data.size())); break;
            }
            const auto& body = std::get<Body>(value.data);
            std::string contents;
            if (type == 'a') contents = value.signature.substr(1);
            else if (type == '(' || type == '{') contents = value.signature.substr(1, value.signature.size() - 2);
            else if (type == 'v' && body.size() == 1) {
                validate_value_signature(body[0].signature);
                contents = body[0].signature;
            }
            else throw std::runtime_error("invalid D-Bus container");
            check(sd_bus_message_open_container(message, type == '(' ? 'r' : type == '{' ? 'e' : type, contents.c_str()));
            std::size_t at = 0;
            for (const auto& child : body) {
                if (at == contents.size()) throw std::runtime_error("D-Bus container arity mismatch");
                const auto end = type == 'a' || type == 'v' ? contents.size() : type_end(contents, at, 0, false);
                if (child.signature != std::string_view(contents).substr(at, end - at))
                    throw std::runtime_error("D-Bus container type mismatch");
                write_value(message, child, budget, depth + 1);
                if (type != 'a') at = end;
            }
            if (type != 'a' && at != contents.size()) throw std::runtime_error("D-Bus container arity mismatch");
            check(sd_bus_message_close_container(message));
        }
    }
}

void write_body(sd_bus_message* message, const Body& body) {
    BodyBudget budget;
    std::size_t signature_bytes = 0;
    for (const auto& value : body) {
        validate_value_signature(value.signature);
        signature_bytes += value.signature.size();
        if (signature_bytes > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
        write_value(message, value, budget, 0);
    }
}

Message encode_property(sd_bus* bus, const Value& value, BodyBudget& budget) {
    validate_value_signature(value.signature);
    auto message = dbus::method(bus, "org.freedesktop.DBus", "/", "org.hyprworks.Property", "Value");
    write_value(message.get(), value, budget, 3);
    check(sd_bus_message_seal(message.get(), 1, 0));
    return message;
}

void property_budget(const BodyBudget& budget) {
    if (budget.bytes > 65536 || budget.nodes > 4096 || budget.fds > 16)
        throw std::length_error("D-Bus retained property budget exceeded");
}

void outgoing_capacity(sd_bus* bus) {
    if (!bus) throw std::runtime_error("D-Bus connection is stopped");
    std::uint64_t count;
    check(sd_bus_get_n_queued_write(bus, &count));
    if (count >= 64) throw std::length_error("D-Bus outgoing message queue is full");
}

}

struct FileDescriptor::Owned {
    int fd;
    explicit Owned(int source) : fd(fcntl(source, F_DUPFD_CLOEXEC, 0)) {
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "duplicate D-Bus FD");
    }
    ~Owned() { close(fd); }
};

std::expected<FileDescriptor, std::string> FileDescriptor::duplicate(int fd) {
    try {
        FileDescriptor result;
        result.fd_ = std::make_shared<Owned>(fd);
        return result;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

int FileDescriptor::get() const noexcept { return fd_ ? fd_->fd : -1; }

std::string_view take_type(std::string_view& signature) {
    if (signature.size() > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
    const auto type = signature.substr(0, type_end(signature, 0, 0, false));
    signature.remove_prefix(type.size());
    return type;
}

struct Connection::State {
    struct Pending {
        State* state;
        std::uint64_t id;
        sd_bus_slot* slot = nullptr;
        bool done = false;
        ~Pending() { sd_bus_slot_unref(slot); }
    };
    struct Subscription {
        State* state;
        std::uint64_t id;
        sd_bus_slot* slot = nullptr;
        ~Subscription() { sd_bus_slot_unref(slot); }
    };
    struct Export {
        struct Property {
            std::string name, signature;
            bool constant;
            BodyBudget budget;
            Message message{nullptr, sd_bus_message_unref};
        };
        State* state;
        std::string path, interface;
        std::vector<Method> methods;
        std::vector<Signal> signals;
        std::vector<Property> properties;
        BodyBudget budget;
        std::vector<sd_bus_vtable> table;
        std::chrono::milliseconds timeout;
        sd_bus_slot* slot = nullptr;
        ~Export() { sd_bus_slot_unref(slot); }
    };
    struct Name {
        State* state;
        std::string name;
        sd_bus_slot* slot = nullptr;
        bool owned = false, releasing = false, finished = false;
        ~Name() { sd_bus_slot_unref(slot); }
    };
    struct Incoming {
        std::uint64_t id;
        Message message;
        std::string output;
        Clock::time_point deadline;
    };
    sd_bus* bus = nullptr;
    sd_bus_slot* owner_slot = nullptr;
    Status status;
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<std::unique_ptr<Subscription>> subscriptions;
    std::vector<std::unique_ptr<Export>> exports;
    std::vector<std::unique_ptr<Name>> names;
    std::vector<Incoming> incoming;
    std::vector<Event> events;
    std::uint64_t last_id = 0, generation = 0;
    std::size_t event_bytes = 0, event_fds = 0;
    std::exception_ptr callback_error;
    std::optional<Clock::time_point> connect_deadline;
    bool more = false;

    Export& export_target(const std::string& path, const std::string& interface) {
        for (const auto& object : exports)
            if (object->path == path && object->interface == interface) return *object;
        throw std::runtime_error("unknown D-Bus exported interface");
    }
    void retire(std::vector<Incoming>::iterator& request, const char* name, const char* reason, bool send) {
        if (send) check(sd_bus_reply_method_errorf(request->message.get(), name, "%s", reason));
        std::erase_if(events, [&](const Event& event) { return event.kind == EventKind::method && event.id == request->id; });
        queue({.kind = EventKind::expired, .id = request->id,
            .sender = sd_bus_message_get_sender(request->message.get()), .error_name = name, .error = reason});
        request = incoming.erase(request);
    }
    auto reply_target(std::uint64_t id) {
        const auto found = std::ranges::find(incoming, id, &Incoming::id);
        if (found == incoming.end() || Clock::now() >= found->deadline)
            throw std::runtime_error("unknown or expired D-Bus incoming request");
        return found;
    }

    std::uint64_t next_id() {
        if (last_id == INT64_MAX) throw std::runtime_error("D-Bus identifiers exhausted");
        return ++last_id;
    }
    void queue(Event event, sd_bus_message* message = nullptr) {
        if (events.size() >= 256) throw std::length_error("D-Bus event queue exceeds 256 entries");
        if (event.path.size() > 16384) throw std::length_error("D-Bus path exceeds 16 KiB");
        std::size_t bytes = event.path.size() + event.interface.size() + event.member.size() +
            event.sender.size() + event.error.size() + event.error_name.size();
        std::size_t fds = 0;
        if (message) {
            const char* sender = sd_bus_message_get_sender(message);
            event.sender = sender ? sender : "";
            bytes += event.sender.size();
            if (const auto* error = sd_bus_message_get_error(message)) {
                event.error_name = error->name;
                const char* detail = error->message ? error->message : error->name;
                event.error.assign(detail, ::strnlen(detail, 16384));
                bytes += event.error.size() + event.error_name.size();
            } else {
                BodyBudget budget;
                while (!check(sd_bus_message_at_end(message, false))) event.arguments.push_back(read_value(message, budget, 0));
                bytes += budget.bytes + budget.nodes * sizeof(Value);
                fds = budget.fds;
            }
        }
        if (bytes > 16 * 1024 * 1024 - event_bytes) throw std::length_error("D-Bus event queue exceeds 16 MiB");
        if (fds > 256 - event_fds) throw std::length_error("D-Bus event queue exceeds 256 file descriptors");
        events.push_back(std::move(event));
        event_bytes += bytes;
        event_fds += fds;
    }
    void disconnect() noexcept {
        incoming.clear(); pending.clear(); subscriptions.clear(); exports.clear(); names.clear();
        owner_slot = sd_bus_slot_unref(owner_slot);
        bus = sd_bus_close_unref(bus); connect_deadline.reset(); more = false;
    }
    static int name_reply(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& name = *static_cast<Name*>(userdata);
        name.finished = true;
        try {
            Event event{.kind = EventKind::name_failed, .member = name.name};
            if (const auto* error = sd_bus_message_get_error(message)) {
                event.error_name = error->name;
                event.error = error->message ? error->message : error->name;
                // A timed-out ownership operation has an uncertain result.
                if (sd_bus_error_has_name(error, SD_BUS_ERROR_NO_REPLY) || sd_bus_error_has_name(error, SD_BUS_ERROR_TIMEOUT) ||
                    sd_bus_error_has_name(error, SD_BUS_ERROR_DISCONNECTED))
                    throw std::runtime_error(event.error_name + ": " + event.error);
                name.releasing = false;
                name.state->queue(std::move(event));
                return 0;
            }
            std::uint32_t result;
            require(sd_bus_message_read(message, "u", &result));
            if (name.releasing) {
                if (name.owned) {
                    name.owned = false;
                    name.state->queue({.kind = EventKind::name_lost, .member = name.name});
                }
            } else if (result == 1 || result == 4) {
                name.owned = true;
                name.state->queue({.kind = EventKind::name_acquired, .member = name.name});
            } else {
                event.error_name = "org.freedesktop.DBus.Error.NameExists";
                event.error = "D-Bus name is already owned";
                name.state->queue(std::move(event));
            }
        } catch (...) { name.state->callback_error = std::current_exception(); }
        return 0;
    }
    static int owner_changed(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(userdata);
        try {
            const char *name, *previous, *current;
            require(sd_bus_message_read(message, "sss", &name, &previous, &current));
            if (name[0] == ':' && previous[0] && !current[0]) {
                for (auto it = state.incoming.begin(); it != state.incoming.end();) {
                    if (std::string_view(sd_bus_message_get_sender(it->message.get())) != name) { ++it; continue; }
                    state.retire(it, SD_BUS_ERROR_DISCONNECTED, "D-Bus caller disconnected", false);
                }
                state.queue({.kind = EventKind::peer_gone, .sender = name});
            }
            const char* unique = nullptr;
            check(sd_bus_get_unique_name(state.bus, &unique));
            for (const auto& owned : state.names) {
                if (owned->name != name || !owned->owned || owned->releasing || std::string_view(current) == unique) continue;
                owned->owned = false;
                state.queue({.kind = EventKind::name_lost, .member = name});
            }
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int owner_match_reply(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(userdata);
        try {
            if (const auto* error = sd_bus_message_get_error(message))
                throw std::runtime_error(std::string(error->name) + ": " + (error->message ? error->message : ""));
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }
    static int property(sd_bus*, const char*, const char*, const char* name, sd_bus_message* reply,
            void* userdata, sd_bus_error* error) noexcept {
        auto& object = *static_cast<Export*>(userdata);
        try {
            const auto found = std::ranges::find(object.properties, name, &Export::Property::name);
            assert(found != object.properties.end());
            check(sd_bus_message_rewind(found->message.get(), true));
            check(sd_bus_message_copy(reply, found->message.get(), true));
            return 1;
        } catch (const std::exception& exception) {
            return sd_bus_error_setf(error, SD_BUS_ERROR_FAILED, "%s", exception.what());
        }
    }
    static int call_reply(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& request = *static_cast<Pending*>(userdata);
        request.done = true;
        try { request.state->queue({.kind = EventKind::reply, .id = request.id}, message); }
        catch (...) { request.state->callback_error = std::current_exception(); }
        return 0;
    }
    static int signal(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& subscription = *static_cast<Subscription*>(userdata);
        try {
            Event event{.kind = EventKind::signal, .id = subscription.id};
            if (const auto* path = sd_bus_message_get_path(message)) event.path = path;
            if (const auto* interface = sd_bus_message_get_interface(message)) event.interface = interface;
            if (const auto* member = sd_bus_message_get_member(message)) event.member = member;
            subscription.state->queue(std::move(event), message);
        } catch (...) { subscription.state->callback_error = std::current_exception(); }
        return 0;
    }
    static int match_reply(sd_bus_message* message, void* userdata, sd_bus_error*) noexcept {
        auto& subscription = *static_cast<Subscription*>(userdata);
        try {
            if (const auto* error = sd_bus_message_get_error(message))
                throw std::runtime_error(error->message ? error->message : error->name);
        } catch (...) { subscription.state->callback_error = std::current_exception(); }
        return 0;
    }
    static int method(sd_bus_message* message, void* userdata, sd_bus_error* error) noexcept {
        auto& object = *static_cast<Export*>(userdata);
        auto& state = *object.state;
        try {
            if (state.incoming.size() >= 32) throw std::length_error("D-Bus incoming request limit reached");
            const std::string member = sd_bus_message_get_member(message);
            const auto found = std::ranges::find(object.methods, member, &Method::name);
            assert(found != object.methods.end());
            const auto id = state.next_id();
            state.incoming.push_back({id, Message(sd_bus_message_ref(message), sd_bus_message_unref), found->output, Clock::now() + object.timeout});
            try {
                state.queue({.kind = EventKind::method, .id = id, .path = object.path, .interface = object.interface, .member = member}, message);
            } catch (...) {
                state.incoming.pop_back();
                throw;
            }
            return 1;
        } catch (const std::exception& exception) {
            return sd_bus_error_setf(error, SD_BUS_ERROR_LIMITS_EXCEEDED, "%s", exception.what());
        }
    }
};

Connection::Connection() : state_(std::make_unique<State>()) {}
Connection::~Connection() { stop(); }

std::expected<void, std::string> Connection::start(bool system, const std::string& address) {
    stop();
    auto& state = *state_;
    try {
        state.pending.reserve(64); state.subscriptions.reserve(64); state.exports.reserve(128);
        state.names.reserve(32); state.incoming.reserve(32); state.events.reserve(256);
        dbus::open(state.bus, address, system);
        check(sd_bus_add_match_async(state.bus, &state.owner_slot,
            "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
            State::owner_changed, State::owner_match_reply, &state));
        state.status = {Phase::connecting, {}};
        state.connect_deadline = Clock::now() + std::chrono::seconds(10);
        return {};
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error);
    }
}

void Connection::stop() noexcept {
    state_->disconnect(); state_->events.clear(); state_->event_bytes = 0; state_->event_fds = 0;
    state_->callback_error = {}; state_->status = {}; ++state_->generation;
}

std::expected<std::uint64_t, std::string> Connection::call(const Call& call) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        if (state.pending.size() >= 64) throw std::length_error("D-Bus outgoing request limit reached");
        if (call.timeout.count() < 1 || call.timeout > std::chrono::minutes(5)) throw std::runtime_error("D-Bus timeout must be 1..300000 milliseconds");
        if (!sd_bus_service_name_is_valid(call.destination.c_str()) || call.destination.contains('\0') ||
            !sd_bus_object_path_is_valid(call.path.c_str()) || call.path.contains('\0') ||
            !sd_bus_interface_name_is_valid(call.interface.c_str()) || call.interface.contains('\0') ||
            !sd_bus_member_name_is_valid(call.member.c_str()) || call.member.contains('\0')) throw std::runtime_error("invalid D-Bus method address");
        auto message = dbus::method(state.bus, call.destination.c_str(), call.path.c_str(), call.interface.c_str(), call.member.c_str());
        write_body(message.get(), call.arguments);
        check(sd_bus_message_set_allow_interactive_authorization(message.get(), call.interactive));
        auto pending = std::make_unique<State::Pending>(&state, state.next_id());
        const auto id = pending->id;
        check(sd_bus_call_async(state.bus, &pending->slot, message.get(), State::call_reply, pending.get(),
            static_cast<std::uint64_t>(call.timeout.count()) * 1000));
        state.pending.push_back(std::move(pending)); ++state.generation;
        return id;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::cancel(std::uint64_t id) {
    auto& state = *state_;
    const auto found = std::ranges::find_if(state.pending, [id](const auto& request) { return request->id == id; });
    if (found == state.pending.end()) return std::unexpected("unknown D-Bus outgoing request");
    state.pending.erase(found);
    std::erase_if(state.events, [id](const auto& event) { return event.kind == EventKind::reply && event.id == id; });
    ++state.generation; return {};
}

std::expected<std::uint64_t, std::string> Connection::subscribe(const std::string& match) {
    auto& state = *state_;
    try {
        if (!state.bus) throw std::runtime_error("D-Bus connection is stopped");
        if (state.subscriptions.size() >= 64 || match.size() > 16384 || match.contains('\0')) throw std::runtime_error("D-Bus subscription limit exceeded");
        auto subscription = std::make_unique<State::Subscription>(&state, state.next_id());
        const auto id = subscription->id;
        check(sd_bus_add_match_async(state.bus, &subscription->slot, match.c_str(), State::signal, State::match_reply, subscription.get()));
        state.subscriptions.push_back(std::move(subscription)); ++state.generation;
        return id;
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::unsubscribe(std::uint64_t id) {
    auto& state = *state_;
    const auto found = std::ranges::find_if(state.subscriptions, [id](const auto& subscription) { return subscription->id == id; });
    if (found == state.subscriptions.end()) return std::unexpected("unknown D-Bus subscription");
    state.subscriptions.erase(found);
    std::erase_if(state.events, [id](const auto& event) { return event.kind == EventKind::signal && event.id == id; });
    ++state.generation; return {};
}

std::expected<void, std::string> Connection::request_name(const std::string& name) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        if (name.empty() || name[0] == ':' || name.contains('\0') || !sd_bus_service_name_is_valid(name.c_str()))
            throw std::runtime_error("invalid D-Bus service name");
        if (state.names.size() >= 32) throw std::length_error("D-Bus owned name limit reached");
        for (const auto& item : state.names)
            if (item->name == name) throw std::runtime_error("D-Bus name is already tracked");
        auto item = std::make_unique<State::Name>(&state, name);
        check(sd_bus_request_name_async(state.bus, &item->slot, name.c_str(), 0, State::name_reply, item.get()));
        state.names.push_back(std::move(item)); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::release_name(const std::string& name) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        for (const auto& item : state.names) {
            if (item->name != name) continue;
            if (!item->owned || item->slot) throw std::runtime_error("D-Bus name operation is pending");
            check(sd_bus_release_name_async(state.bus, &item->slot, name.c_str(), State::name_reply, item.get()));
            item->releasing = true; item->finished = false; ++state.generation;
            return {};
        }
        throw std::runtime_error("unknown D-Bus owned name");
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::vector<std::string> Connection::owned_names() const {
    std::vector<std::string> result;
    for (const auto& name : state_->names) if (name->owned) result.push_back(name->name);
    return result;
}

std::expected<void, std::string> Connection::export_interface(const std::string& path, const std::string& interface,
        const std::vector<Method>& methods, const std::vector<Signal>& signals,
        const std::vector<Property>& properties, std::chrono::milliseconds timeout) {
    auto& state = *state_;
    try {
        if (!state.bus) throw std::runtime_error("D-Bus connection is stopped");
        if (state.exports.size() >= 128 || methods.size() > 32 || signals.size() > 32 || properties.size() > 32 ||
            (methods.empty() && signals.empty() && properties.empty())) throw std::length_error("D-Bus export limit exceeded");
        if (path.size() > 16384 || !sd_bus_object_path_is_valid(path.c_str()) || path.contains('\0') || !sd_bus_interface_name_is_valid(interface.c_str()) ||
            interface.contains('\0') || timeout.count() < 1 || timeout > std::chrono::minutes(5)) throw std::runtime_error("invalid D-Bus export");
        for (const auto& object : state.exports)
            if (object->path == path && object->interface == interface) throw std::runtime_error("D-Bus interface already exported");
        auto object = std::make_unique<State::Export>();
        object->state = &state; object->path = path; object->interface = interface; object->methods = methods; object->timeout = timeout;
        object->signals = signals;
        object->properties.reserve(properties.size());
        for (const auto& property : properties) {
            if (!sd_bus_member_name_is_valid(property.name.c_str()) || property.name.contains('\0'))
                throw std::runtime_error("invalid D-Bus property name");
            State::Export::Property retained{property.name, property.value.signature, property.constant, {}};
            retained.message = encode_property(state.bus, property.value, retained.budget);
            object->budget.bytes += retained.budget.bytes;
            object->budget.nodes += retained.budget.nodes;
            object->budget.fds += retained.budget.fds;
            property_budget(object->budget);
            object->properties.push_back(std::move(retained));
        }
        object->table.push_back(SD_BUS_VTABLE_START(0));
        for (const auto& method : object->methods) {
            if (!sd_bus_member_name_is_valid(method.name.c_str()) || method.name.contains('\0')) throw std::runtime_error("invalid D-Bus method name");
            for (std::string_view signature : {std::string_view(method.input), std::string_view(method.output)})
                while (!signature.empty()) take_type(signature);
            object->table.push_back(SD_BUS_METHOD(method.name.c_str(), method.input.c_str(), method.output.c_str(), State::method, SD_BUS_VTABLE_UNPRIVILEGED));
        }
        for (const auto& signal : object->signals) {
            if (!sd_bus_member_name_is_valid(signal.name.c_str()) || signal.name.contains('\0'))
                throw std::runtime_error("invalid D-Bus signal name");
            std::string_view signature = signal.signature;
            while (!signature.empty()) take_type(signature);
            object->table.push_back(SD_BUS_SIGNAL(signal.name.c_str(), signal.signature.c_str(), 0));
        }
        for (const auto& property : object->properties) {
            object->table.push_back(SD_BUS_PROPERTY(property.name.c_str(), property.signature.c_str(), State::property, 0,
                property.constant ? SD_BUS_VTABLE_PROPERTY_CONST : SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE));
        }
        object->table.push_back(SD_BUS_VTABLE_END);
        check(sd_bus_add_object_vtable(state.bus, &object->slot, path.c_str(), interface.c_str(), object->table.data(), object.get()));
        state.exports.push_back(std::move(object)); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::unexport(const std::string& path, const std::string& interface) {
    auto& state = *state_;
    try {
        auto& object = state.export_target(path, interface);
        for (auto it = state.incoming.begin(); it != state.incoming.end();) {
            if (path != sd_bus_message_get_path(it->message.get()) || interface != sd_bus_message_get_interface(it->message.get())) {
                ++it; continue;
            }
            state.retire(it, SD_BUS_ERROR_UNKNOWN_OBJECT, "D-Bus interface removed", true);
        }
        std::erase_if(state.exports, [&](const auto& item) { return item.get() == &object; });
        ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::emit(const std::string& path, const std::string& interface,
        const std::string& member, const Body& arguments, const std::string& destination) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        const auto& object = state.export_target(path, interface);
        const auto found = std::ranges::find(object.signals, member, &Signal::name);
        if (found == object.signals.end()) throw std::runtime_error("unknown D-Bus exported signal");
        if (!destination.empty() && (destination.contains('\0') || !sd_bus_service_name_is_valid(destination.c_str())))
            throw std::runtime_error("invalid D-Bus signal destination");
        sd_bus_message* raw = nullptr;
        check(sd_bus_message_new_signal(state.bus, &raw, path.c_str(), interface.c_str(), member.c_str()));
        Message message(raw, sd_bus_message_unref);
        if (!destination.empty()) check(sd_bus_message_set_destination(raw, destination.c_str()));
        write_body(raw, arguments);
        if (found->signature != sd_bus_message_get_signature(raw, true))
            throw std::runtime_error("D-Bus signal signature does not match declaration");
        check(sd_bus_send(state.bus, raw, nullptr)); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::set_property(const std::string& path, const std::string& interface,
        const std::string& name, const Value& value) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        auto& object = state.export_target(path, interface);
        const auto found = std::ranges::find(object.properties, name, &State::Export::Property::name);
        if (found == object.properties.end()) throw std::runtime_error("unknown D-Bus exported property");
        if (found->constant || found->signature != value.signature)
            throw std::runtime_error("constant property or mismatched D-Bus property signature");
        BodyBudget candidate_budget;
        auto candidate = encode_property(state.bus, value, candidate_budget);
        const BodyBudget budget{
            object.budget.nodes - found->budget.nodes + candidate_budget.nodes,
            object.budget.bytes - found->budget.bytes + candidate_budget.bytes,
            object.budget.fds - found->budget.fds + candidate_budget.fds,
        };
        property_budget(budget);
        sd_bus_message* raw = nullptr;
        check(sd_bus_message_new_signal(state.bus, &raw, path.c_str(), "org.freedesktop.DBus.Properties", "PropertiesChanged"));
        Message message(raw, sd_bus_message_unref);
        check(sd_bus_message_append(raw, "s", interface.c_str()));
        check(sd_bus_message_open_container(raw, 'a', "{sv}"));
        check(sd_bus_message_open_container(raw, 'e', "sv"));
        check(sd_bus_message_append(raw, "s", name.c_str()));
        check(sd_bus_message_open_container(raw, 'v', found->signature.c_str()));
        check(sd_bus_message_copy(raw, candidate.get(), true));
        check(sd_bus_message_close_container(raw));
        check(sd_bus_message_close_container(raw));
        check(sd_bus_message_close_container(raw));
        check(sd_bus_message_open_container(raw, 'a', "s"));
        check(sd_bus_message_close_container(raw));
        check(sd_bus_send(state.bus, raw, nullptr));
        found->message = std::move(candidate); found->budget = candidate_budget;
        object.budget = budget; ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::reply(std::uint64_t id, const Body& arguments) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        const auto found = state.reply_target(id);
        sd_bus_message* raw = nullptr; check(sd_bus_message_new_method_return(found->message.get(), &raw));
        Message message(raw, sd_bus_message_unref);
        write_body(message.get(), arguments);
        if (found->output != sd_bus_message_get_signature(message.get(), true))
            throw std::runtime_error("D-Bus reply signature does not match exported method");
        check(sd_bus_send(state.bus, message.get(), nullptr));
        state.incoming.erase(found); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::error(std::uint64_t id, const std::string& name, const std::string& message) {
    auto& state = *state_;
    try {
        outgoing_capacity(state.bus);
        if (!sd_bus_interface_name_is_valid(name.c_str()) || name.contains('\0') || message.size() > 16384 || message.contains('\0'))
            throw std::runtime_error("invalid D-Bus error");
        const auto found = state.reply_target(id);
        check(sd_bus_reply_method_errorf(found->message.get(), name.c_str(), "%s", message.c_str()));
        state.incoming.erase(found); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

WaitSet Connection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation;
    if (!state.bus) return wait;
    wait.deadline = state.connect_deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    for (const auto& request : state.incoming)
        if (!wait.deadline || request.deadline < *wait.deadline) wait.deadline = request.deadline;
    if (state.more || !state.events.empty()) wait.deadline = Clock::time_point::min();
    return wait;
}

Update Connection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        return {};
    ++state.generation;
    const auto before = state.status;
    if (!state.bus) return {};
    try {
        state.more = dbus::process(state.bus, state.callback_error);
        std::erase_if(state.pending, [](const auto& request) { return request->done; });
        for (const auto& name : state.names)
            if (name->finished) name->slot = sd_bus_slot_unref(name->slot);
        std::erase_if(state.names, [](const auto& name) { return name->finished && !name->owned; });
        const auto now = Clock::now();
        if (sd_bus_is_ready(state.bus) > 0) { state.status = {Phase::ready, {}}; state.connect_deadline.reset(); }
        if (state.connect_deadline && now >= *state.connect_deadline) throw std::runtime_error("D-Bus connection timed out");
        for (auto it = state.incoming.begin(); it != state.incoming.end();) {
            if (now < it->deadline) { ++it; continue; }
            state.retire(it, SD_BUS_ERROR_NO_REPLY, "Application response timed out", true);
        }
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()}; state.events.clear();
    }
    state.event_bytes = 0; state.event_fds = 0;
    return {before != state.status, std::exchange(state.events, {})};
}

const Status& Connection::status() const noexcept { return state_->status; }

}
