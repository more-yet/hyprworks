#include <hyprworks/commands.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace hyprworks::commands {
namespace {
constexpr char path[] = "/org/hyprworks/Commands", interface[] = "org.hyprworks.Commands1";
using dbus::check;
using dbus::require;
}

struct CommandServer::State {
    struct Pending {
        std::uint64_t id;
        dbus::Message message;
        Clock::time_point deadline;
    };
    sd_bus* bus = nullptr;
    sd_bus_slot *object = nullptr, *ownership = nullptr;
    Status status;
    std::vector<std::string> commands;
    std::vector<Pending> pending;
    std::vector<Request> requests;
    std::optional<Clock::time_point> deadline;
    std::uint64_t generation = 0, last_id = 0;
    bool more = false, status_pending = false;
    std::exception_ptr callback_error;

    void release() noexcept {
        pending.clear(); requests.clear(); deadline.reset(); more = false; callback_error = {};
        ownership = sd_bus_slot_unref(ownership); object = sd_bus_slot_unref(object);
        bus = sd_bus_close_unref(bus);
    }
    static int acquired(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            std::uint32_t reply; require(sd_bus_message_read(message, "u", &reply));
            if (reply != 1 && reply != 4) throw std::runtime_error("command bus name is already owned");
            state.status = {Phase::ready, {}}; state.deadline.reset();
        } catch (...) { state.callback_error = std::current_exception(); }
        state.status_pending = true; return 1;
    }
    static int list(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            sd_bus_message* raw = nullptr; check(sd_bus_message_new_method_return(message, &raw));
            dbus::Message reply(raw, sd_bus_message_unref);
            check(sd_bus_message_open_container(reply.get(), 'a', "s"));
            for (const auto& command : state.commands) check(sd_bus_message_append(reply.get(), "s", command.c_str()));
            check(sd_bus_message_close_container(reply.get())); check(sd_bus_send(state.bus, reply.get(), nullptr)); return 1;
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_FAILED, "%s", exception.what()); }
    }
    static int invoke(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (state.status.phase != Phase::ready) return sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "command server is not ready");
            if (state.pending.size() == 32) return sd_bus_error_set_const(error, SD_BUS_ERROR_LIMITS_EXCEEDED, "command queue is full");
            Request request; std::size_t bytes = 0;
            request.name = dbus::text(message, 's', bytes);
            if (!std::ranges::binary_search(state.commands, request.name)) return sd_bus_error_set_const(error, SD_BUS_ERROR_INVALID_ARGS, "unknown command");
            request.sender = sd_bus_message_get_sender(message);
            require(sd_bus_message_enter_container(message, 'a', "s"));
            while (!check(sd_bus_message_at_end(message, false))) {
                if (request.arguments.size() == 64) throw std::runtime_error("command exceeds 64 arguments");
                request.arguments.push_back(dbus::text(message, 's', bytes));
                if (bytes > 65536) throw std::runtime_error("command exceeds 64 KiB");
            }
            check(sd_bus_message_exit_container(message));
            if (state.last_id == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("command request identifiers exhausted");
            request.id = ++state.last_id;
            state.pending.reserve(state.pending.size() + 1); state.requests.reserve(state.requests.size() + 1);
            state.pending.push_back({request.id, dbus::Message(sd_bus_message_ref(message), sd_bus_message_unref), Clock::now() + std::chrono::seconds(5)});
            state.requests.push_back(std::move(request)); return 1;
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s", exception.what()); }
    }
};

CommandServer::CommandServer() : state_(std::make_unique<State>()) {}
CommandServer::~CommandServer() { state_->release(); }
const Status& CommandServer::status() const noexcept { return state_->status; }
void CommandServer::stop() noexcept { state_->release(); state_->status = {}; state_->status_pending = true; ++state_->generation; }

std::expected<void, std::string> CommandServer::start(const std::string& name, std::span<const std::string> commands, const std::string& address) {
    if (name.empty() || name.size() > 255 || name.contains('\0') || name[0] == ':' || !sd_bus_service_name_is_valid(name.c_str()) || commands.empty() || commands.size() > 128)
        return std::unexpected("invalid command bus name or command count (1..128)");
    std::vector<std::string> names(commands.begin(), commands.end());
    for (const auto& command : names)
        if (command.empty() || command.size() > 128 || command.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
            return std::unexpected("command names must be 1..128 ASCII letters, digits, underscores, dots or hyphens");
    std::ranges::sort(names);
    if (std::ranges::adjacent_find(names) != names.end()) return std::unexpected("duplicate command name");
    stop(); auto& state = *state_; state.commands = std::move(names);
    try {
        dbus::open(state.bus, address);
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("List", "", "as", State::list, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Invoke", "sas", "s", State::invoke, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        check(sd_bus_add_object_vtable(state.bus, &state.object, path, interface, vtable, &state));
        state.status = {Phase::connecting, {}}; state.status_pending = true; state.deadline = Clock::now() + std::chrono::seconds(5);
        check(sd_bus_request_name_async(state.bus, &state.ownership, name.c_str(), 0, State::acquired, &state));
        return {};
    } catch (const std::exception& error) { state.release(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error); }
}

WaitSet CommandServer::prepare_wait() {
    auto& state = *state_; WaitSet wait; wait.generation = ++state.generation;
    wait.deadline = state.deadline;
    if (state.bus) dbus::prepare(state.bus, wait.fd, wait.deadline);
    for (const auto& pending : state.pending) if (!wait.deadline || pending.deadline < *wait.deadline) wait.deadline = pending.deadline;
    if (state.more || state.status_pending) wait.deadline = Clock::time_point::min();
    return wait;
}

Update CommandServer::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        throw std::logic_error("dispatch requires the current command wait set");
    ++state.generation;
    if (state.bus) try {
        state.more = dbus::process(state.bus, state.callback_error);
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline) throw std::runtime_error("command name acquisition timed out");
        for (std::size_t i = 0; i < state.pending.size();) {
            const auto& pending = state.pending[i];
            if (pending.deadline > now) { ++i; continue; }
            check(sd_bus_reply_method_errorf(pending.message.get(), SD_BUS_ERROR_TIMEOUT, "command response timed out"));
            std::erase_if(state.requests, [&](const auto& request) { return request.id == pending.id; });
            state.pending.erase(state.pending.begin() + static_cast<std::ptrdiff_t>(i));
        }
    } catch (const std::exception& error) { state.release(); state.status = {Phase::failed, error.what()}; state.status_pending = true; }
    return {std::exchange(state.status_pending, false), std::exchange(state.requests, {})};
}

std::expected<void, std::string> CommandServer::respond(std::uint64_t id, const std::string& text, bool success) {
    auto& state = *state_;
    if (text.size() > 65536 || text.contains('\0')) return std::unexpected("command response exceeds 64 KiB or contains NUL");
    const auto found = std::ranges::find(state.pending, id, &State::Pending::id);
    if (found == state.pending.end()) return std::unexpected("unknown or expired command request");
    if (Clock::now() >= found->deadline) return std::unexpected("command request has expired");
    const int result = success ? sd_bus_reply_method_return(found->message.get(), "s", text.c_str()) :
        sd_bus_reply_method_errorf(found->message.get(), "org.hyprworks.Commands1.Rejected", "%s", text.c_str());
    if (result < 0) return std::unexpected(std::string("send command response: ") + std::strerror(-result));
    state.pending.erase(found); ++state.generation; return {};
}
}
