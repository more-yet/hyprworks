#include <hyprworks/polkit.hpp>
#include "dbus.hpp"
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#include <systemd/sd-login.h>
#include <algorithm>
#include <cassert>
#include <climits>
#include <signal.h>

namespace hyprworks::polkit {
namespace {

constexpr char authority[] = "org.freedesktop.PolicyKit1";
constexpr char authority_path[] = "/org/freedesktop/PolicyKit1/Authority";
constexpr char authority_interface[] = "org.freedesktop.PolicyKit1.Authority";
constexpr char agent_path[] = "/org/hyprworks/AuthenticationAgent";
constexpr char agent_interface[] = "org.freedesktop.PolicyKit1.AuthenticationAgent";
constexpr char cancelled_error[] = "org.freedesktop.PolicyKit1.Error.Cancelled";
using dbus::check;
using dbus::require;

// Suppress libpolkit's synchronous SIGPIPE without consuming an already-pending signal.
struct PipeSignals {
    sigset_t mask{}, previous{};
    bool pending = false;
    PipeSignals() {
        sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
        const int error = pthread_sigmask(SIG_BLOCK, &mask, &previous);
        if (error) throw std::system_error(error, std::generic_category(), "block helper SIGPIPE");
        sigset_t signals{};
        sigpending(&signals); pending = sigismember(&signals, SIGPIPE);
    }
    ~PipeSignals() {
        if (!pending) {
            const timespec timeout{};
            while (sigtimedwait(&mask, nullptr, &timeout) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};

}

struct Agent::State {
    sd_bus* bus = nullptr;
    sd_bus_slot *object = nullptr, *registration = nullptr, *owner_watch = nullptr;
    GMainContext* context = nullptr;
    std::array<GPollFD, 8> polls{};
    int poll_count = 0, priority = 0;
    PolkitAgentSession* session = nullptr;
    dbus::Message call{nullptr, sd_bus_message_unref};
    Status status;
    std::string owner, cookie;
    std::shared_ptr<const Request> request;
    std::vector<Event> events;
    std::optional<Clock::time_point> deadline;
    std::uint64_t generation = 0, last_id = 0, last_prompt = 0, prompt = 0;
    std::uint32_t uid = 0;
    unsigned attempts = 0;
    bool more = false, status_pending = false, session_done = false, authorized = false;
    std::exception_ptr callback_error;

    void release_session() noexcept {
        if (!session) return;
        g_signal_handlers_disconnect_by_data(session, this);
        polkit_agent_session_cancel(session);
        g_object_unref(std::exchange(session, nullptr));
        session_done = false; prompt = 0;
    }

    void release() noexcept {
        release_session();
        if (call) sd_bus_reply_method_errorf(call.get(), cancelled_error, "Authentication cancelled");
        call.reset(); request.reset(); cookie.clear(); events.clear(); owner.clear();
        registration = sd_bus_slot_unref(registration);
        object = sd_bus_slot_unref(object); owner_watch = sd_bus_slot_unref(owner_watch);
        bus = sd_bus_close_unref(bus);
        if (context) {
            g_main_context_release(context);
            g_main_context_unref(std::exchange(context, nullptr));
        }
        deadline.reset(); poll_count = 0; more = false; callback_error = {};
    }

    void fail(const std::string& error) {
        const auto id = request ? request->id : 0;
        std::erase_if(events, [id](const auto& event) { return event.kind != EventKind::completed || event.id == id; });
        auto completed = std::move(events);
        release();
        events = std::move(completed);
        status = {Phase::failed, error}; status_pending = true; ++generation;
        if (id) events.push_back({.kind = EventKind::completed, .id = id, .request = {}, .text = {}, .cancelled = true});
    }

    void message(EventKind kind, const char* text, bool echo = false) {
        if (events.size() == 32) throw std::runtime_error("authentication event limit reached (32)");
        const auto length = strnlen(text, 16385);
        if (length > 16384 || !g_utf8_validate(text, static_cast<gssize>(length), nullptr))
            throw std::runtime_error("invalid authentication message");
        if (kind == EventKind::prompt) {
            if (prompt) throw std::runtime_error("authentication helper requested overlapping responses");
            if (last_prompt == UINT64_MAX) throw std::runtime_error("authentication prompt identifiers exhausted");
            prompt = ++last_prompt;
        }
        events.push_back({.kind = kind, .id = request->id, .prompt = kind == EventKind::prompt ? prompt : 0,
            .request = {}, .text = std::string(text, length), .echo = echo});
    }

    void initiate() {
        release_session();
        auto* identity = polkit_unix_user_new(static_cast<int>(uid));
        session = polkit_agent_session_new(identity, cookie.c_str());
        g_object_unref(identity);
        g_signal_connect(session, "request", G_CALLBACK(+[](PolkitAgentSession*, const char* text, gboolean echo, void* data) noexcept {
            auto& state = *static_cast<State*>(data);
            try { state.message(EventKind::prompt, text, echo); }
            catch (...) { state.callback_error = std::current_exception(); }
        }), this);
        g_signal_connect(session, "show-info", G_CALLBACK(+[](PolkitAgentSession*, const char* text, void* data) noexcept {
            auto& state = *static_cast<State*>(data);
            try { state.message(EventKind::info, text); }
            catch (...) { state.callback_error = std::current_exception(); }
        }), this);
        g_signal_connect(session, "show-error", G_CALLBACK(+[](PolkitAgentSession*, const char* text, void* data) noexcept {
            auto& state = *static_cast<State*>(data);
            try { state.message(EventKind::error, text); }
            catch (...) { state.callback_error = std::current_exception(); }
        }), this);
        g_signal_connect(session, "completed", G_CALLBACK(+[](PolkitAgentSession*, gboolean authorized, void* data) noexcept {
            auto& state = *static_cast<State*>(data);
            state.session_done = true; state.authorized = authorized; state.prompt = 0;
        }), this);
        ++attempts;
        const PipeSignals signals;
        g_main_context_push_thread_default(context);
        polkit_agent_session_initiate(session);
        g_main_context_pop_thread_default(context);
    }

    void finish(bool success, bool cancelled) {
        const auto id = request->id;
        std::erase_if(events, [id](const auto& event) { return event.id == id && event.kind == EventKind::prompt; });
        if (events.size() == 32) throw std::runtime_error("authentication event limit reached (32)");
        events.push_back({.kind = EventKind::completed, .id = id, .request = {}, .text = {},
            .authorized = success, .cancelled = cancelled});
        release_session();
        if (cancelled) check(sd_bus_reply_method_errorf(call.get(), cancelled_error, "Authentication cancelled"));
        else check(sd_bus_reply_method_return(call.get(), ""));
        call.reset(); request.reset(); cookie.clear(); deadline.reset(); ++generation;
    }

    static int registered(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message))
                throw std::runtime_error(error->message ? error->message : error->name);
            state.owner = sd_bus_message_get_sender(message);
            state.status = {Phase::ready, {}}; state.deadline.reset(); state.status_pending = true;
        } catch (...) { state.callback_error = std::current_exception(); }
        return 1;
    }

    static int owner_changed(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            const char *name, *old_owner, *new_owner;
            require(sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner));
            if (!state.owner.empty() && state.owner == old_owner && state.owner != new_owner)
                throw std::runtime_error("Polkit authority disconnected; restart the agent to register again");
        } catch (...) { state.callback_error = std::current_exception(); }
        return 1;
    }

    bool trusted(sd_bus_message* message) const {
        return status.phase == Phase::ready && owner == sd_bus_message_get_sender(message);
    }

    static int begin(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        if (!state.trusted(message)) return sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED, "Only the registered Polkit authority may authenticate");
        if (state.request) return sd_bus_error_set_const(error, "org.freedesktop.PolicyKit1.Error.Failed", "An authentication request is already active");
        try {
            auto request = std::make_shared<Request>();
            std::size_t bytes = 0;
            request->action = dbus::text(message, 's', bytes);
            request->message = dbus::text(message, 's', bytes);
            request->icon = dbus::text(message, 's', bytes);
            require(sd_bus_message_enter_container(message, 'a', "{ss}"));
            while (check(sd_bus_message_enter_container(message, 'e', "ss"))) {
                if (request->details.size() == 64) throw std::runtime_error("authentication details exceed 64 entries");
                auto key = dbus::text(message, 's', bytes);
                auto value = dbus::text(message, 's', bytes);
                request->details.emplace_back(std::move(key), std::move(value));
                check(sd_bus_message_exit_container(message));
            }
            check(sd_bus_message_exit_container(message));
            auto cookie = dbus::text(message, 's', bytes);
            if (cookie.empty() || cookie.size() > 4096 || cookie.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("invalid authentication cookie");
            require(sd_bus_message_enter_container(message, 'a', "(sa{sv})"));
            unsigned count = 0;
            while (check(sd_bus_message_enter_container(message, 'r', "sa{sv}"))) {
                if (++count > 32) throw std::runtime_error("authentication identities exceed 32 entries");
                const auto kind = dbus::text(message, 's', bytes);
                require(sd_bus_message_enter_container(message, 'a', "{sv}"));
                std::optional<std::uint32_t> uid;
                unsigned fields = 0;
                while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
                    if (++fields > 16) throw std::runtime_error("authentication identity has too many fields");
                    const auto key = dbus::text(message, 's', bytes);
                    if (kind == "unix-user" && key == "uid") {
                        if (uid) throw std::runtime_error("duplicate identity uid");
                        require(sd_bus_message_enter_container(message, 'v', "u"));
                        std::uint32_t value; require(sd_bus_message_read(message, "u", &value));
                        uid = value;
                        check(sd_bus_message_exit_container(message));
                    } else check(sd_bus_message_skip(message, "v"));
                    check(sd_bus_message_exit_container(message));
                }
                check(sd_bus_message_exit_container(message));
                check(sd_bus_message_exit_container(message));
                if (kind == "unix-user" && (!uid || *uid > INT_MAX)) throw std::runtime_error("invalid Unix user identity");
                if (kind == "unix-user" && std::ranges::find(request->identities, *uid, &Identity::uid) == request->identities.end()) {
                    auto* identity = polkit_unix_user_new(static_cast<int>(*uid));
                    const char* name = polkit_unix_user_get_name(POLKIT_UNIX_USER(identity));
                    Identity user{*uid, {}};
                    try { if (name) user.name = name; }
                    catch (...) { g_object_unref(identity); throw; }
                    g_object_unref(identity);
                    request->identities.push_back(std::move(user));
                }
            }
            check(sd_bus_message_exit_container(message));
            if (bytes > 65536 || request->action.empty() || request->identities.empty())
                throw std::runtime_error("invalid authentication request or metadata exceeds 64 KiB");
            if (state.events.size() == 32) throw std::runtime_error("authentication event limit reached (32)");
            if (state.last_id == UINT64_MAX) throw std::runtime_error("authentication request identifiers exhausted");
            request->id = ++state.last_id;
            state.events.push_back({.kind = EventKind::request, .id = request->id, .request = request, .text = {}});
            state.request = std::move(request); state.cookie = std::move(cookie);
            state.call.reset(sd_bus_message_ref(message)); state.attempts = 0;
            state.deadline = Clock::now() + std::chrono::minutes(5);
            return 1;
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s", exception.what()); }
    }

    static int cancel_request(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        if (!state.trusted(message)) return sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED, "Only the registered Polkit authority may cancel authentication");
        try {
            const char* cookie; require(sd_bus_message_read(message, "s", &cookie));
            if (!state.request || state.cookie != cookie)
                return sd_bus_error_set_const(error, "org.freedesktop.PolicyKit1.Error.Failed", "No matching authentication request");
            state.finish(false, true);
            return sd_bus_reply_method_return(message, "");
        } catch (...) { state.callback_error = std::current_exception(); return sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "Cannot cancel authentication"); }
    }
};

Agent::Agent() : state_(std::make_unique<State>()) {}
Agent::~Agent() { state_->release(); }
const Status& Agent::status() const noexcept { return state_->status; }
bool Agent::pending(std::uint64_t id, std::uint64_t prompt) const noexcept {
    const auto& state = *state_;
    return state.request && state.request->id == id && (!prompt || state.prompt == prompt);
}
void Agent::stop() noexcept {
    state_->release(); state_->status = {}; state_->status_pending = true; ++state_->generation;
}

std::expected<void, std::string> Agent::start(const Options& options, const std::string& address) {
    if (options.session.size() > 256 || options.locale.size() > 256 ||
        options.session.contains('\0') || options.locale.contains('\0'))
        return std::unexpected("invalid Polkit session or locale");
    stop(); auto& state = *state_;
    try {
        std::string session = options.session, locale = options.locale;
        if (session.empty()) {
            char* name = nullptr;
            if (sd_pid_get_session(0, &name) < 0 && sd_uid_get_display(getuid(), &name) < 0)
                throw std::runtime_error("cannot find the login session; provide a Polkit session ID");
            const std::unique_ptr<char, decltype(&free)> cleanup(name, free);
            session = name;
        }
        if (locale.empty()) {
            for (const auto* key : {"LC_ALL", "LC_MESSAGES", "LANG"})
                if (const char* value = getenv(key); value && *value) { locale = value; break; }
            if (locale.empty()) locale = "C";
            if (locale.size() > 256) throw std::runtime_error("Polkit locale exceeds 256 bytes");
        }
        dbus::open(state.bus, address, true);
        state.context = g_main_context_new();
        const bool acquired = g_main_context_acquire(state.context);
        assert(acquired);
        (void)acquired;
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("BeginAuthentication", "sssa{ss}sa(sa{sv})", "", State::begin, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CancelAuthentication", "s", "", State::cancel_request, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        check(sd_bus_add_object_vtable(state.bus, &state.object, agent_path, agent_interface, vtable, &state));
        check(sd_bus_add_match(state.bus, &state.owner_watch,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.PolicyKit1'",
            State::owner_changed, &state));
        auto message = dbus::method(state.bus, authority, authority_path, authority_interface, "RegisterAuthenticationAgent");
        check(sd_bus_message_append(message.get(), "(sa{sv})ss", "unix-session", 1, "session-id", "s", session.c_str(), locale.c_str(), agent_path));
        check(sd_bus_call_async(state.bus, &state.registration, message.get(), State::registered, &state, 5000000));
        state.status = {Phase::connecting, {}}; state.status_pending = true;
        state.deadline = Clock::now() + std::chrono::seconds(5);
        return {};
    } catch (const std::exception& error) { state.fail(error.what()); return std::unexpected(state.status.error); }
}

WaitSet Agent::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation; wait.deadline = state.deadline;
    if (state.bus) {
        dbus::prepare(state.bus, wait.fds[0], wait.deadline);
        const bool ready = g_main_context_prepare(state.context, &state.priority);
        int timeout = -1;
        state.poll_count = g_main_context_query(state.context, state.priority, &timeout, state.polls.data(), static_cast<int>(state.polls.size()));
        if (state.poll_count > static_cast<int>(state.polls.size())) throw std::runtime_error("Polkit GLib descriptor limit exceeded");
        for (int i = 0; i < state.poll_count; ++i) {
            const auto& fd = state.polls[static_cast<std::size_t>(i)];
            wait.fds[static_cast<std::size_t>(i) + 1] = {fd.fd, static_cast<short>(fd.events), 0};
        }
        if (ready) wait.deadline = Clock::time_point::min();
        else if (timeout >= 0) {
            const auto at = Clock::now() + std::chrono::milliseconds(timeout);
            if (!wait.deadline || at < *wait.deadline) wait.deadline = at;
        }
    }
    if (state.more || state.status_pending || !state.events.empty() || state.session_done || state.callback_error)
        wait.deadline = Clock::time_point::min();
    return wait;
}

Update Agent::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fds[0].fd != (state.bus ? sd_bus_get_fd(state.bus) : -1)) return {};
    ++state.generation;
    if (state.bus) try {
        state.more = dbus::process(state.bus, state.callback_error);
        for (int i = 0; i < state.poll_count; ++i)
            state.polls[static_cast<std::size_t>(i)].revents = static_cast<gushort>(ready.fds[static_cast<std::size_t>(i) + 1].revents);
        if (g_main_context_check(state.context, state.priority, state.polls.data(), state.poll_count))
            g_main_context_dispatch(state.context);
        if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, {}));
        if (state.deadline && Clock::now() >= *state.deadline) {
            if (!state.request) throw std::runtime_error("Polkit registration timed out");
            state.message(EventKind::error, "Authentication timed out");
            state.finish(false, true);
        } else if (state.session_done) {
            if (state.authorized || state.attempts == 3) state.finish(state.authorized, false);
            else {
                state.message(EventKind::error, "Authentication failed. Please try again.");
                state.initiate();
            }
        }
    } catch (const std::exception& error) { state.fail(error.what()); }
    return {std::exchange(state.status_pending, false), std::exchange(state.events, {})};
}

std::expected<void, std::string> Agent::authenticate(std::uint64_t id, std::uint32_t uid) {
    auto& state = *state_;
    if (!pending(id)) return std::unexpected("authentication request is no longer active");
    if (state.session) return std::unexpected("authentication is already in progress");
    if (std::ranges::find(state.request->identities, uid, &Identity::uid) == state.request->identities.end())
        return std::unexpected("identity was not offered by Polkit");
    try {
        state.uid = uid; state.initiate(); ++state.generation;
        return {};
    } catch (const std::exception& error) { state.fail(error.what()); return std::unexpected(state.status.error); }
}

std::expected<void, std::string> Agent::respond(std::uint64_t id, std::uint64_t prompt, std::string_view response) {
    auto& state = *state_;
    if (!prompt || !pending(id, prompt)) return std::unexpected("authentication prompt is no longer active");
    if (response.size() > 4095 || response.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos)
        return std::unexpected("authentication response exceeds 4095 bytes or contains NUL/newline");
    try {
        const PipeSignals signals;
        char text[4096];
        if (!response.empty()) std::memcpy(text, response.data(), response.size());
        text[response.size()] = '\0';
        state.prompt = 0; ++state.generation;
        polkit_agent_session_response(state.session, text);
        explicit_bzero(text, response.size() + 1);
        return {};
    } catch (const std::exception& error) { state.fail(error.what()); return std::unexpected(state.status.error); }
}

std::expected<void, std::string> Agent::cancel(std::uint64_t id) {
    auto& state = *state_;
    if (!pending(id)) return std::unexpected("authentication request is no longer active");
    try { state.finish(false, true); return {}; }
    catch (const std::exception& error) { state.fail(error.what()); return std::unexpected(state.status.error); }
}

}
