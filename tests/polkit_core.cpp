#include "../src/runtime/invocation.hpp"
#include "../src/native/dbus.hpp"
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <signal.h>

using namespace hyprworks;
using dbus::check;
using dbus::require;

namespace {

PolkitAgentSession* helper = nullptr;
GMainContext* context = nullptr;
guint pending_source = 0;
unsigned initiated = 0, cancelled = 0;
bool broken_pipe = false;
enum class Signal { prompt, echo, success, failure };
Signal next_signal = Signal::prompt;

void schedule(Signal signal) {
    assert(helper && context && !pending_source);
    next_signal = signal;
    auto* source = g_idle_source_new();
    g_source_set_callback(source, +[](void* data) -> gboolean {
        pending_source = 0;
        auto* session = static_cast<PolkitAgentSession*>(data);
        if (next_signal == Signal::prompt) {
            g_signal_emit_by_name(session, "show-info", "Authentication required");
            g_signal_emit_by_name(session, "request", "Password:", FALSE);
        } else if (next_signal == Signal::echo) g_signal_emit_by_name(session, "request", "Token:", TRUE);
        else g_signal_emit_by_name(session, "completed", next_signal == Signal::success);
        return G_SOURCE_REMOVE;
    }, g_object_ref(helper), g_object_unref);
    pending_source = g_source_attach(source, context);
    g_source_unref(source);
}

struct Reply {
    bool done = false;
    std::string error;
    static int received(sd_bus_message* message, void* data, sd_bus_error*) {
        auto& reply = *static_cast<Reply*>(data);
        if (const auto* error = sd_bus_message_get_error(message)) reply.error = error->name;
        reply.done = true;
        return 1;
    }
};

struct Authority {
    sd_bus* bus = nullptr;
    sd_bus_slot* object = nullptr;
    std::string agent;
    bool reject = false;
    Authority() {
        dbus::open(bus, {});
        check(sd_bus_request_name(bus, "org.freedesktop.PolicyKit1", 0));
        static const sd_bus_vtable methods[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("RegisterAuthenticationAgent", "(sa{sv})ss", "", (+[](sd_bus_message* message, void* data, sd_bus_error* error) {
                auto& self = *static_cast<Authority*>(data);
                if (self.reject) return sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "Agent already registered");
                const char *kind, *key, *session, *locale, *path;
                require(sd_bus_message_enter_container(message, 'r', "sa{sv}"));
                require(sd_bus_message_read(message, "s", &kind));
                assert(std::string_view(kind) == "unix-session");
                require(sd_bus_message_enter_container(message, 'a', "{sv}"));
                require(sd_bus_message_enter_container(message, 'e', "sv"));
                require(sd_bus_message_read(message, "s", &key));
                assert(std::string_view(key) == "session-id");
                require(sd_bus_message_enter_container(message, 'v', "s"));
                require(sd_bus_message_read(message, "s", &session));
                assert(std::string_view(session) == "test");
                check(sd_bus_message_exit_container(message));
                check(sd_bus_message_exit_container(message));
                check(sd_bus_message_exit_container(message));
                check(sd_bus_message_exit_container(message));
                require(sd_bus_message_read(message, "ss", &locale, &path));
                assert(*locale && std::string_view(path) == "/org/hyprworks/AuthenticationAgent");
                self.agent = sd_bus_message_get_sender(message);
                return sd_bus_reply_method_return(message, "");
            }), SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        check(sd_bus_add_object_vtable(bus, &object, "/org/freedesktop/PolicyKit1/Authority",
            "org.freedesktop.PolicyKit1.Authority", methods, this));
    }
    ~Authority() { sd_bus_slot_unref(object); sd_bus_close_unref(bus); }
    void process() {
        std::exception_ptr error;
        dbus::process(bus, error);
    }
    void begin(Reply& reply, const char* cookie = "test-cookie", sd_bus* sender = nullptr) {
        if (!sender) sender = bus;
        auto message = dbus::method(sender, agent.c_str(), "/org/hyprworks/AuthenticationAgent",
            "org.freedesktop.PolicyKit1.AuthenticationAgent", "BeginAuthentication");
        check(sd_bus_message_append(message.get(), "sssa{ss}sa(sa{sv})",
            "org.hyprworks.Test", "Authenticate for the test", "dialog-password", 1, "test", "value", cookie,
            1, "unix-user", 1, "uid", "u", static_cast<unsigned>(getuid())));
        check(sd_bus_call_async(sender, nullptr, message.get(), Reply::received, &reply, 5000000));
        check(sd_bus_flush(sender));
    }
    void cancel(Reply& reply, const char* cookie = "test-cookie") {
        auto message = dbus::method(bus, agent.c_str(), "/org/hyprworks/AuthenticationAgent",
            "org.freedesktop.PolicyKit1.AuthenticationAgent", "CancelAuthentication");
        check(sd_bus_message_append(message.get(), "s", cookie));
        check(sd_bus_call_async(bus, nullptr, message.get(), Reply::received, &reply, 5000000));
        check(sd_bus_flush(bus));
    }
};

polkit::Update turn(polkit::Agent& agent, Authority& authority) {
    authority.process();
    auto wait = agent.prepare_wait();
    assert(poll(wait.fds.data(), wait.fds.size(), 1) >= 0);
    auto update = agent.dispatch(wait);
    authority.process();
    return update;
}

template<class Done>
void until(polkit::Agent& agent, Authority& authority, Done done) {
    const auto end = polkit::Clock::now() + std::chrono::seconds(3);
    while (!done()) {
        assert(polkit::Clock::now() < end);
        turn(agent, authority);
    }
}

polkit::Event event(polkit::Agent& agent, Authority& authority, polkit::EventKind kind) {
    const auto end = polkit::Clock::now() + std::chrono::seconds(3);
    for (;;) {
        assert(polkit::Clock::now() < end);
        auto update = turn(agent, authority);
        for (auto& event : update.events) if (event.kind == kind) return std::move(event);
    }
}

}

extern "C" void __wrap_polkit_agent_session_initiate(PolkitAgentSession* session) {
    assert(!helper);
    helper = session;
    context = g_main_context_get_thread_default();
    assert(context && context != g_main_context_default());
    ++initiated;
    schedule(Signal::prompt);
}
extern "C" void __wrap_polkit_agent_session_response(PolkitAgentSession* session, const char* response) {
    assert(session == helper);
    if (broken_pipe) {
        sigset_t mask{};
        assert(pthread_sigmask(SIG_BLOCK, nullptr, &mask) == 0 && sigismember(&mask, SIGPIPE));
        assert(raise(SIGPIPE) == 0);
    }
    schedule(std::string_view(response) == "next" ? Signal::echo :
        std::string_view(response) == "good" ? Signal::success : Signal::failure);
}
extern "C" void __wrap_polkit_agent_session_cancel(PolkitAgentSession* session) {
    assert(session == helper);
    if (pending_source) {
        auto* source = g_main_context_find_source_by_id(context, pending_source);
        assert(source);
        g_source_destroy(source); pending_source = 0;
    }
    helper = nullptr; context = nullptr; ++cancelled;
    g_signal_emit_by_name(session, "completed", FALSE);
}

int main() {
    Authority authority;
    const std::string address = getenv("DBUS_SESSION_BUS_ADDRESS");
    const auto uid = static_cast<std::uint32_t>(getuid());
    {
        polkit::Agent agent;
        assert(agent.start({.session = "test", .locale = "C"}, address));
        until(agent, authority, [&] { return agent.status().phase == polkit::Phase::ready; });
        Reply reply;
        authority.begin(reply);
        auto request = event(agent, authority, polkit::EventKind::request);
        assert(request.request && request.request->identities.size() == 1 && request.request->identities[0].uid == uid);
        assert(!agent.authenticate(request.id, uid + 1));
        auto stale = agent.prepare_wait();
        assert(agent.authenticate(request.id, uid));
        assert(agent.dispatch(stale).events.empty());
        assert(!agent.authenticate(request.id, uid));
        auto prompt = event(agent, authority, polkit::EventKind::prompt);
        assert(prompt.id == request.id && prompt.prompt && !prompt.echo);
        assert(!agent.respond(request.id, prompt.prompt + 1, "good"));
        assert(!agent.respond(request.id, prompt.prompt, "line\nbreak"));
        assert(!agent.respond(request.id, prompt.prompt, std::string(4096, 'x')));
        assert(agent.respond(request.id, prompt.prompt, "next"));
        assert(!agent.respond(request.id, prompt.prompt, "good"));
        auto echo = event(agent, authority, polkit::EventKind::prompt);
        assert(echo.echo && echo.prompt != prompt.prompt);
        broken_pipe = true;
        assert(agent.respond(request.id, echo.prompt, "good"));
        broken_pipe = false;
        sigset_t signals{};
        assert(sigpending(&signals) == 0 && !sigismember(&signals, SIGPIPE));
        auto completed = event(agent, authority, polkit::EventKind::completed);
        assert(completed.authorized && !completed.cancelled && !helper && !agent.pending(request.id));
        until(agent, authority, [&] { return reply.done; });
        assert(reply.error.empty() && request.request->message == "Authenticate for the test");
        assert(!agent.cancel(request.id));

        Reply failure;
        authority.begin(failure);
        request = event(agent, authority, polkit::EventKind::request);
        assert(agent.authenticate(request.id, uid));
        const auto started = initiated;
        for (int attempt = 0; attempt < 3; ++attempt) {
            prompt = event(agent, authority, polkit::EventKind::prompt);
            assert(!agent.respond(request.id, echo.prompt, "good"));
            assert(agent.respond(request.id, prompt.prompt, attempt == 1 ? std::string_view{} : "bad"));
        }
        completed = event(agent, authority, polkit::EventKind::completed);
        assert(!completed.authorized && !completed.cancelled && initiated == started + 2 && !helper);
        until(agent, authority, [&] { return failure.done; });
        assert(failure.error.empty());

        Reply waiting, busy;
        authority.begin(waiting);
        request = event(agent, authority, polkit::EventKind::request);
        authority.begin(busy, "second-cookie");
        until(agent, authority, [&] { return busy.done; });
        assert(!busy.error.empty() && agent.pending(request.id));
        sd_bus* impostor = nullptr;
        dbus::open(impostor, address);
        Reply spoof;
        authority.begin(spoof, "spoof", impostor);
        const auto end = polkit::Clock::now() + std::chrono::seconds(3);
        while (!spoof.done) {
            assert(polkit::Clock::now() < end);
            turn(agent, authority);
            std::exception_ptr error;
            dbus::process(impostor, error);
        }
        assert(spoof.error == SD_BUS_ERROR_ACCESS_DENIED);
        sd_bus_close_unref(impostor);
        assert(agent.authenticate(request.id, uid));
        prompt = event(agent, authority, polkit::EventKind::prompt);
        Reply cancel;
        authority.cancel(cancel);
        completed = event(agent, authority, polkit::EventKind::completed);
        assert(completed.cancelled && !helper && !agent.respond(request.id, prompt.prompt, "good"));
        until(agent, authority, [&] { return cancel.done && waiting.done; });
        assert(cancel.error.empty() && waiting.error == "org.freedesktop.PolicyKit1.Error.Cancelled");

        Reply racing, cancellation;
        authority.begin(racing);
        request = event(agent, authority, polkit::EventKind::request);
        assert(agent.authenticate(request.id, uid) && pending_source);
        auto ready = agent.prepare_wait();
        authority.cancel(cancellation);
        assert(poll(ready.fds.data(), 1, 1000) == 1);
        auto update = agent.dispatch(ready);
        assert(std::ranges::none_of(update.events, [](const auto& event) { return event.kind == polkit::EventKind::prompt; }));
        assert(std::ranges::any_of(update.events, [](const auto& event) { return event.kind == polkit::EventKind::completed && event.cancelled; }));
        assert(!helper && !pending_source);
        until(agent, authority, [&] { return cancellation.done && racing.done; });

        Reply abandoned;
        authority.begin(abandoned);
        request = event(agent, authority, polkit::EventKind::request);
        assert(agent.authenticate(request.id, uid) && pending_source);
        stale = agent.prepare_wait();
        agent.stop();
        assert(!helper && !pending_source && agent.dispatch(stale).events.empty());
        assert(agent.start({.session = "test", .locale = "C"}, address));
        until(agent, authority, [&] { return agent.status().phase == polkit::Phase::ready; });
        assert(!agent.pending(request.id) && !agent.respond(request.id, prompt.prompt, "good"));
        Reply disconnected;
        authority.begin(disconnected);
        request = event(agent, authority, polkit::EventKind::request);
        assert(agent.authenticate(request.id, uid));
        prompt = event(agent, authority, polkit::EventKind::prompt);
        check(sd_bus_release_name(authority.bus, "org.freedesktop.PolicyKit1"));
        completed = event(agent, authority, polkit::EventKind::completed);
        assert(completed.cancelled && agent.status().phase == polkit::Phase::failed);
        assert(!agent.status().error.empty() && !helper && !pending_source);
        assert(!agent.respond(request.id, prompt.prompt, "good"));
        check(sd_bus_request_name(authority.bus, "org.freedesktop.PolicyKit1", 0));
        assert(agent.start({.session = "test", .locale = "C"}, address));
        until(agent, authority, [&] { return agent.status().phase == polkit::Phase::ready; });
        Reply previous, incoming;
        authority.begin(previous);
        request = event(agent, authority, polkit::EventKind::request);
        assert(agent.cancel(request.id));
        authority.begin(incoming);
        check(sd_bus_release_name(authority.bus, "org.freedesktop.PolicyKit1"));
        update = turn(agent, authority);
        assert(agent.status().phase == polkit::Phase::failed && update.events.size() == 2);
        assert(update.events[0].id == request.id && update.events[0].cancelled);
        assert(update.events[1].kind == polkit::EventKind::completed && update.events[1].id != request.id && update.events[1].cancelled);
        check(sd_bus_request_name(authority.bus, "org.freedesktop.PolicyKit1", 0));
        authority.reject = true;
        assert(agent.start({.session = "test", .locale = "C"}, address));
        until(agent, authority, [&] { return agent.status().phase == polkit::Phase::failed; });
        assert(agent.status().error.find("already registered") != std::string::npos);
        authority.reject = false;
    }
    {
        assert(setenv("DBUS_SYSTEM_BUS_ADDRESS", address.c_str(), 1) == 0);
        lua::InvocationState state;
        state.lua = luaL_newstate();
        assert(state.lua);
        *static_cast<lua::InvocationState**>(lua_getextraspace(state.lua)) = &state;
        luaL_openlibs(state.lua);
        lua_newtable(state.lua); lua::register_polkit(state.lua); lua_setglobal(state.lua, "hw");
        const auto script = [&](const char* source) {
            if (luaL_dostring(state.lua, source) != LUA_OK) {
                std::cerr << lua_tostring(state.lua, -1) << '\n'; std::abort();
            }
        };
        script(R"(
            agent = assert(hw.polkit {session = "test", locale = "C"})
            assert(hw.polkit() == agent)
            events = {}
            agent:on_event(function(handle, event)
                assert(handle == agent and event.cookie == nil)
                events[#events + 1] = event.kind
                if event.kind == "request" then
                    request_id = event.id
                    assert(handle:authenticate(event.id, event.identities[1].uid))
                elseif event.kind == "prompt" then
                    assert(not event.echo)
                    assert(handle:respond(event.id, event.prompt, "good"))
                elseif event.kind == "completed" then
                    assert(event.authorized and not event.cancelled)
                    handle:close()
                end
            end)
        )");
        auto& binding = *state.polkit;
        until(binding.agent, authority, [&] { return binding.agent.status().phase == polkit::Phase::ready; });
        Reply reply;
        authority.begin(reply);
        const auto end = polkit::Clock::now() + std::chrono::seconds(3);
        while (!binding.closed) {
            assert(polkit::Clock::now() < end);
            auto update = turn(binding.agent, authority);
            binding.status_pending |= update.status_changed; binding.events = std::move(update.events);
            assert(lua::dispatch_polkit(state.lua, binding) == 0);
        }
        assert(!helper);
        script("assert(events[1] == 'request' and events[#events] == 'completed'); assert(agent:status().phase == 'closed')");
        script(R"(
            assert(agent:restart())
            agent:on_event(function() error("cancelled request delivered") end)
        )");
        until(binding.agent, authority, [&] { return binding.agent.status().phase == polkit::Phase::ready; });
        Reply pending;
        authority.begin(pending);
        auto request = event(binding.agent, authority, polkit::EventKind::request);
        lua::identifier(state.lua, request.id); lua_setglobal(state.lua, "request_id");
        script("agent:on_status(function(handle) assert(handle:cancel(request_id)) end)");
        binding.events.push_back(std::move(request)); binding.status_pending = true;
        assert(lua::dispatch_polkit(state.lua, binding) == 0 && !helper);
        script("agent:close()");
        script("assert(agent:restart())");
        until(binding.agent, authority, [&] { return binding.agent.status().phase == polkit::Phase::ready; });
        Reply unattended;
        authority.begin(unattended);
        binding.events.push_back(event(binding.agent, authority, polkit::EventKind::request));
        assert(lua::dispatch_polkit(state.lua, binding) == 0 && !helper);
        until(binding.agent, authority, [&] { return unattended.done; });
        assert(unattended.error == "org.freedesktop.PolicyKit1.Error.Cancelled");
        script("agent:close()");
        state.closing = true;
        lua_close(state.lua); state.lua = nullptr;
        state.polkit.reset();
    }
    assert(initiated == cancelled && !helper && !pending_source);
    std::cout << "Polkit: registration, caller ownership, sessions, retries, stale prompts, cancellation, shutdown and Lua delivery passed\n";
    return 0;
}
