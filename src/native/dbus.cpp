#include <hyprworks/dbus.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace hyprworks::dbus {
namespace {

std::size_t type_end(std::string_view signature, std::size_t at, unsigned depth, bool dictionary) {
    if (depth > 32 || at >= signature.size()) throw std::runtime_error("invalid D-Bus signature");
    const char type = signature[at++];
    if (std::string_view("ybnqiuxtdsogv").contains(type)) return at;
    if (type == 'a') return type_end(signature, at, depth + 1, true);
    if (type == '(') {
        const auto begin = at;
        while (at < signature.size() && signature[at] != ')') at = type_end(signature, at, depth + 1, false);
        if (at == begin || at == signature.size()) throw std::runtime_error("invalid D-Bus struct signature");
        return at + 1;
    }
    if (type == '{' && dictionary) {
        if (at == signature.size() || !std::string_view("ybnqiuxtdsog").contains(signature[at++]))
            throw std::runtime_error("invalid D-Bus dictionary key");
        at = type_end(signature, at, depth + 1, false);
        if (at == signature.size() || signature[at] != '}') throw std::runtime_error("invalid D-Bus dictionary signature");
        return at + 1;
    }
    throw std::runtime_error("unsupported D-Bus type");
}

struct Budget {
    std::size_t nodes = 0, bytes = 0;
    void add(unsigned depth, std::size_t size = 0) {
        if (depth > 32 || ++nodes > 65536 || size > 8 * 1024 * 1024 || (bytes += size) > 8 * 1024 * 1024)
            throw std::length_error("D-Bus message exceeds depth, value or 8 MiB payload limit");
    }
};

void validate_value_signature(std::string_view signature) {
    if (signature.size() > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
    if (type_end(signature, 0, 0, false) != signature.size())
        throw std::runtime_error("D-Bus value requires one complete type");
}

Value read_value(sd_bus_message* message, Budget& budget, unsigned depth) {
    budget.add(depth);
    char type;
    const char* contents = nullptr;
    require(sd_bus_message_peek_type(message, &type, &contents));
    Value value;
    value.signature = std::string(1, type);
    switch (type) {
        case 'b': { int data; require(sd_bus_message_read_basic(message, type, &data)); value.data = data != 0; break; }
        case 'y': { std::uint8_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 'q': { std::uint16_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 'u': { std::uint32_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::uint64_t(data); break; }
        case 't': { std::uint64_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = data; break; }
        case 'n': { std::int16_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::int64_t(data); break; }
        case 'i': { std::int32_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = std::int64_t(data); break; }
        case 'x': { std::int64_t data; require(sd_bus_message_read_basic(message, type, &data)); value.data = data; break; }
        case 'd': {
            double data; require(sd_bus_message_read_basic(message, type, &data));
            if (!std::isfinite(data)) throw std::runtime_error("nonfinite D-Bus number");
            value.data = data; break;
        }
        case 's': case 'o': case 'g': {
            const char* data; require(sd_bus_message_read_basic(message, type, &data));
            const auto size = std::strlen(data); budget.add(depth, size);
            value.data = std::string(data, size); break;
        }
        case 'a': case 'r': case 'e': case 'v': {
            if (type == 'a') value.signature += contents;
            else if (type == 'r') value.signature = "(" + std::string(contents) + ")";
            else if (type == 'e') value.signature = "{" + std::string(contents) + "}";
            if (value.signature == "ay") {
                const void* data; std::size_t size;
                require(sd_bus_message_read_array(message, 'y', &data, &size));
                budget.add(depth, size);
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
        default: throw std::runtime_error("D-Bus Unix file descriptors are not supported");
    }
    return value;
}

void write_value(sd_bus_message* message, const Value& value, Budget& budget, unsigned depth) {
    budget.add(depth);
    // Root and variant signatures are validated; other children match their parent's type.
    const char type = value.signature[0];
    switch (type) {
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
            budget.add(depth, data.size());
            if (data.contains('\0')) throw std::runtime_error("D-Bus text contains NUL");
            check(sd_bus_message_append_basic(message, type, data.c_str())); break;
        }
        default: {
            if (value.signature == "ay") {
                const auto& data = std::get<std::string>(value.data);
                budget.add(depth, data.size());
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
    Budget budget;
    std::size_t signature_bytes = 0;
    for (const auto& value : body) {
        validate_value_signature(value.signature);
        signature_bytes += value.signature.size();
        if (signature_bytes > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
        write_value(message, value, budget, 0);
    }
}

}

std::vector<std::string> split_signature(const std::string& signature, bool dictionary) {
    if (signature.size() > 255) throw std::runtime_error("D-Bus signature exceeds 255 bytes");
    std::vector<std::string> result;
    for (std::size_t at = 0; at < signature.size();) {
        const auto end = type_end(signature, at, 0, dictionary);
        result.push_back(signature.substr(at, end - at)); at = end;
    }
    return result;
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
        State* state;
        std::string path, interface;
        std::vector<Method> methods;
        std::vector<sd_bus_vtable> table;
        std::chrono::milliseconds timeout;
        sd_bus_slot* slot = nullptr;
        ~Export() { sd_bus_slot_unref(slot); }
    };
    struct Incoming {
        std::uint64_t id;
        Message message;
        std::string output;
        Clock::time_point deadline;
    };
    sd_bus* bus = nullptr;
    Status status;
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<std::unique_ptr<Subscription>> subscriptions;
    std::vector<std::unique_ptr<Export>> exports;
    std::vector<Incoming> incoming;
    std::vector<Event> events;
    std::uint64_t last_id = 0, generation = 0;
    std::size_t event_bytes = 0;
    std::exception_ptr callback_error;
    std::optional<Clock::time_point> connect_deadline;
    bool more = false;

    std::uint64_t next_id() {
        if (last_id == INT64_MAX) throw std::runtime_error("D-Bus identifiers exhausted");
        return ++last_id;
    }
    void queue(Event event, sd_bus_message* message = nullptr) {
        if (events.size() >= 256) throw std::length_error("D-Bus event queue exceeds 256 entries");
        if (message) {
            const char* sender = sd_bus_message_get_sender(message);
            event.sender = sender ? sender : "";
            if (const auto* error = sd_bus_message_get_error(message)) {
                event.error_name = error->name;
                const char* detail = error->message ? error->message : error->name;
                event.error.assign(detail, ::strnlen(detail, 16384));
                event_bytes += event.error.size() + event.error_name.size();
            } else {
                Budget budget;
                while (!check(sd_bus_message_at_end(message, false))) event.arguments.push_back(read_value(message, budget, 0));
                event_bytes += budget.bytes + budget.nodes * sizeof(Value);
            }
        }
        if (event.path.size() > 16384) throw std::length_error("D-Bus path exceeds 16 KiB");
        event_bytes += event.path.size() + event.interface.size() + event.member.size() + event.sender.size();
        if (event_bytes > 16 * 1024 * 1024) throw std::length_error("D-Bus event queue exceeds 16 MiB");
        events.push_back(std::move(event));
    }
    void disconnect() noexcept {
        incoming.clear(); pending.clear(); subscriptions.clear(); exports.clear();
        bus = sd_bus_close_unref(bus); connect_deadline.reset(); more = false;
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
            if (found == object.methods.end()) throw std::logic_error("exported method is missing");
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
        dbus::open(state.bus, address, system);
        state.status = {Phase::connecting, {}};
        state.connect_deadline = Clock::now() + std::chrono::seconds(10);
        return {};
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error);
    }
}

void Connection::stop() noexcept {
    state_->disconnect(); state_->events.clear(); state_->event_bytes = 0;
    state_->callback_error = {}; state_->status = {}; ++state_->generation;
}

std::expected<std::uint64_t, std::string> Connection::call(const Call& call) {
    auto& state = *state_;
    try {
        if (!state.bus) throw std::runtime_error("D-Bus connection is stopped");
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

std::expected<void, std::string> Connection::export_methods(const std::string& path, const std::string& interface,
        const std::vector<Method>& methods, std::chrono::milliseconds timeout) {
    auto& state = *state_;
    try {
        if (!state.bus) throw std::runtime_error("D-Bus connection is stopped");
        if (state.exports.size() >= 16 || methods.empty() || methods.size() > 32) throw std::length_error("D-Bus export limit exceeded");
        if (!sd_bus_object_path_is_valid(path.c_str()) || path.contains('\0') || !sd_bus_interface_name_is_valid(interface.c_str()) ||
            interface.contains('\0') || timeout.count() < 1 || timeout > std::chrono::minutes(5)) throw std::runtime_error("invalid D-Bus export");
        for (const auto& object : state.exports)
            if (object->path == path && object->interface == interface) throw std::runtime_error("D-Bus interface already exported");
        auto object = std::make_unique<State::Export>();
        object->state = &state; object->path = path; object->interface = interface; object->methods = methods; object->timeout = timeout;
        object->table.push_back(SD_BUS_VTABLE_START(0));
        for (const auto& method : object->methods) {
            if (!sd_bus_member_name_is_valid(method.name.c_str()) || method.name.contains('\0')) throw std::runtime_error("invalid D-Bus method name");
            split_signature(method.input); split_signature(method.output);
            object->table.push_back(SD_BUS_METHOD(method.name.c_str(), method.input.c_str(), method.output.c_str(), State::method, SD_BUS_VTABLE_UNPRIVILEGED));
        }
        object->table.push_back(SD_BUS_VTABLE_END);
        check(sd_bus_add_object_vtable(state.bus, &object->slot, path.c_str(), interface.c_str(), object->table.data(), object.get()));
        state.exports.push_back(std::move(object)); ++state.generation;
        return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> Connection::reply(std::uint64_t id, const Body& arguments) {
    auto& state = *state_;
    try {
        const auto found = std::ranges::find(state.incoming, id, &State::Incoming::id);
        if (found == state.incoming.end()) throw std::runtime_error("unknown or expired D-Bus incoming request");
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
        if (!sd_bus_interface_name_is_valid(name.c_str()) || name.contains('\0') || message.size() > 16384 || message.contains('\0'))
            throw std::runtime_error("invalid D-Bus error");
        const auto found = std::ranges::find(state.incoming, id, &State::Incoming::id);
        if (found == state.incoming.end()) throw std::runtime_error("unknown or expired D-Bus incoming request");
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
        const auto now = Clock::now();
        if (sd_bus_is_ready(state.bus) > 0) { state.status = {Phase::ready, {}}; state.connect_deadline.reset(); }
        if (state.connect_deadline && now >= *state.connect_deadline) throw std::runtime_error("D-Bus connection timed out");
        for (auto it = state.incoming.begin(); it != state.incoming.end();) {
            if (now < it->deadline) { ++it; continue; }
            check(sd_bus_reply_method_errorf(it->message.get(), SD_BUS_ERROR_NO_REPLY, "Application response timed out"));
            state.queue({.kind = EventKind::expired, .id = it->id});
            it = state.incoming.erase(it);
        }
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()}; state.events.clear();
    }
    state.event_bytes = 0;
    return {before != state.status, std::exchange(state.events, {})};
}

const Status& Connection::status() const noexcept { return state_->status; }

}
