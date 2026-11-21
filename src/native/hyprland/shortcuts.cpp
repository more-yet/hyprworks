#include <hyprworks/hyprland/shortcuts.hpp>
#include "hyprland-global-shortcuts-v1-client.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <wayland-client.h>

namespace hyprworks::hyprland::shortcuts {

struct ShortcutConnection::State {
    struct Registration {
        State* owner = nullptr;
        Shortcut shortcut;
        hyprland_global_shortcut_v1* proxy = nullptr;
    };
    wl_display* display = nullptr;
    bool owns_display = false;
    std::function<void()> wake;
    wl_registry* registry = nullptr;
    wl_callback* sync = nullptr;
    hyprland_global_shortcuts_manager_v1* manager = nullptr;
    std::uint32_t manager_name = 0;
    std::vector<Registration> registrations;
    std::vector<Event> events;
    std::string app_id;
    Status status;
    bool registered = false, pending_status = false;
    const char* callback_error = nullptr;
    std::optional<Clock::time_point> deadline;
    std::uint64_t generation = 0;

    void release() noexcept {
        if (sync) wl_callback_destroy(std::exchange(sync, nullptr));
        for (auto& registration : registrations)
            if (registration.proxy) hyprland_global_shortcut_v1_destroy(std::exchange(registration.proxy, nullptr));
        if (manager) hyprland_global_shortcuts_manager_v1_destroy(std::exchange(manager, nullptr));
        if (registry) wl_registry_destroy(std::exchange(registry, nullptr));
        if (display && owns_display) wl_display_disconnect(display);
        display = nullptr; owns_display = false;
        wake = {};
        registrations.clear(); events.clear(); deadline.reset();
        registered = false; manager_name = 0; callback_error = nullptr;
    }

    void fail(std::string error) {
        release();
        status = {Phase::failed, std::move(error)}; pending_status = true;
    }

    void fail_display() {
        const int error = wl_display_get_error(display);
        std::string message = std::string("shortcut Wayland connection: ") + std::strerror(error ? error : EIO);
        if (error == EPROTO) {
            const wl_interface* interface = nullptr;
            std::uint32_t object = 0;
            const auto code = wl_display_get_protocol_error(display, &interface, &object);
            message += " (" + std::string(interface ? interface->name : "unknown") + " object " + std::to_string(object) + ", code " + std::to_string(code) + ")";
        }
        fail(std::move(message));
    }

    static void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) noexcept {
        auto& state = *static_cast<State*>(data);
        if (state.manager || std::strcmp(interface, hyprland_global_shortcuts_manager_v1_interface.name) != 0 || version < 1) return;
        state.manager_name = name;
        state.manager = static_cast<hyprland_global_shortcuts_manager_v1*>(wl_registry_bind(registry, name, &hyprland_global_shortcuts_manager_v1_interface, 1));
        if (!state.manager) state.callback_error = "cannot bind global shortcuts protocol";
    }

    static void removed(void* data, wl_registry*, std::uint32_t name) noexcept {
        auto& state = *static_cast<State*>(data);
        if (name == state.manager_name) state.callback_error = "global shortcuts protocol was removed";
        if (state.wake) state.wake();
    }

    template<bool Pressed>
    static void event(void* data, hyprland_global_shortcut_v1*, std::uint32_t hi, std::uint32_t lo, std::uint32_t ns) noexcept {
        auto& registration = *static_cast<Registration*>(data);
        auto& state = *registration.owner;
        if (state.callback_error) return;
        if (ns >= 1000000000) state.callback_error = "invalid shortcut timestamp";
        else if (state.events.size() >= 256) state.callback_error = "shortcut event limit exceeded";
        else {
            try {
                state.events.push_back({registration.shortcut.id, Pressed, (static_cast<std::uint64_t>(hi) << 32) | lo, ns});
            } catch (...) { state.callback_error = "cannot retain shortcut event"; }
        }
        if (state.wake) state.wake();
    }

    static void synchronized(void* data, wl_callback* callback, std::uint32_t) noexcept {
        auto& state = *static_cast<State*>(data);
        wl_callback_destroy(callback); state.sync = nullptr;
        if (state.callback_error) return;
        if (!state.manager) { state.callback_error = "compositor does not support Hyprland global shortcuts"; return; }
        if (state.registered) {
            state.status.phase = Phase::ready; state.pending_status = true; state.deadline.reset();
            if (state.wake) state.wake();
            return;
        }
        static const hyprland_global_shortcut_v1_listener listener{event<true>, event<false>};
        for (auto& registration : state.registrations) {
            const auto& shortcut = registration.shortcut;
            registration.proxy = hyprland_global_shortcuts_manager_v1_register_shortcut(state.manager, shortcut.id.c_str(), state.app_id.c_str(), shortcut.description.c_str(), shortcut.trigger_description.c_str());
            if (!registration.proxy || hyprland_global_shortcut_v1_add_listener(registration.proxy, &listener, &registration) < 0) {
                state.callback_error = "cannot register global shortcut"; return;
            }
        }
        state.registered = true;
        state.begin_sync();
    }

    void begin_sync() noexcept {
        static const wl_callback_listener listener{synchronized};
        sync = wl_display_sync(display);
        if (!sync || wl_callback_add_listener(sync, &listener, this) < 0) callback_error = "cannot synchronize global shortcuts";
    }

    std::expected<void, std::string> subscribe(const std::string& app_id, std::span<const Shortcut> shortcuts) {
        this->app_id = app_id;
        registrations.reserve(shortcuts.size());
        for (const auto& shortcut : shortcuts) registrations.push_back({this, shortcut, nullptr});
        static const wl_registry_listener listener{global, removed};
        registry = wl_display_get_registry(display);
        if (!registry || wl_registry_add_listener(registry, &listener, this) < 0) {
            fail("cannot observe Wayland globals"); return std::unexpected(status.error);
        }
        begin_sync();
        if (callback_error) { fail(callback_error); return std::unexpected(status.error); }
        status = {Phase::connecting, {}}; pending_status = true;
        deadline = Clock::now() + std::chrono::seconds(2);
        return {};
    }
};

ShortcutConnection::ShortcutConnection() : state_(std::make_unique<State>()) {}
ShortcutConnection::~ShortcutConnection() { state_->release(); }

static std::expected<void, std::string> validate_shortcuts(const std::string& app_id, std::span<const Shortcut> shortcuts) {
    const auto valid = [](const std::string& text, std::size_t limit, bool empty) {
        return (empty || !text.empty()) && text.size() <= limit && !text.contains('\0');
    };
    if (!valid(app_id, 255, false) || shortcuts.empty() || shortcuts.size() > 128)
        return std::unexpected("invalid shortcut application or count (1..128)");
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
        const auto& shortcut = shortcuts[i];
        if (!valid(shortcut.id, 255, false) || !valid(shortcut.description, 4096, true) || !valid(shortcut.trigger_description, 4096, true))
            return std::unexpected("invalid shortcut id or description");
        for (std::size_t j = 0; j < i; ++j)
            if (shortcuts[j].id == shortcut.id) return std::unexpected("duplicate shortcut id");
    }
    return {};
}

std::expected<void, std::string> ShortcutConnection::start(const std::string& app_id, std::span<const Shortcut> shortcuts, const std::string& display) {
    if (auto result = validate_shortcuts(app_id, shortcuts); !result) return result;
    if (display.size() > 4096 || display.contains('\0')) return std::unexpected("invalid shortcut display");
    stop();
    auto& state = *state_;
    state.owns_display = true;
    state.display = wl_display_connect(display.empty() ? nullptr : display.c_str());
    if (!state.display) {
        state.fail(std::string("connect shortcut Wayland display: ") + std::strerror(errno));
        return std::unexpected(state.status.error);
    }
    return state.subscribe(app_id, shortcuts);
}

std::expected<void, std::string> ShortcutConnection::attach(wl_display* display, const std::string& app_id, std::span<const Shortcut> shortcuts,
                                                          std::function<void()> wake) {
    if (auto result = validate_shortcuts(app_id, shortcuts); !result) return result;
    if (!display || !wake) return std::unexpected("shared shortcut display requires a display and wake callback");
    stop();
    auto& state = *state_;
    state.display = display;
    state.wake = std::move(wake);
    return state.subscribe(app_id, shortcuts);
}

void ShortcutConnection::stop() noexcept {
    auto& state = *state_; ++state.generation;
    state.release(); state.status = {}; state.pending_status = true;
}

WaitSet ShortcutConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation;
    if (state.display && state.owns_display) {
        wait.fd = {wl_display_get_fd(state.display), POLLIN, 0};
        if (wl_display_flush(state.display) < 0) {
            if (errno == EAGAIN) wait.fd.events |= POLLOUT;
            else { state.fail_display(); wait.fd = {-1, 0, 0}; }
        }
    }
    wait.deadline = state.pending_status || state.callback_error || !state.events.empty() ? Clock::time_point::min() : state.deadline;
    return wait;
}

Update ShortcutConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.display && state.owns_display ? wl_display_get_fd(state.display) : -1))
        return {};
    ++state.generation;
    if (state.display && state.owns_display) {
        if (wl_display_dispatch_pending(state.display) < 0) state.fail_display();
        if (state.display && (ready.fd.revents & (POLLIN | POLLHUP | POLLERR))) {
            if (wl_display_prepare_read(state.display) < 0) throw std::logic_error("shortcut event queue was not drained");
            if (wl_display_read_events(state.display) < 0 || wl_display_dispatch_pending(state.display) < 0) state.fail_display();
        }
        if (state.display && (ready.fd.revents & POLLNVAL)) state.fail("invalid shortcut display descriptor");
    }
    if (state.callback_error) state.fail(state.callback_error);
    if (state.deadline && Clock::now() >= *state.deadline) state.fail("global shortcut registration timed out");
    Update update{std::exchange(state.pending_status, false), {}};
    if (state.status.phase == Phase::ready) update.events = std::exchange(state.events, {});
    return update;
}

const Status& ShortcutConnection::status() const noexcept { return state_->status; }

}
