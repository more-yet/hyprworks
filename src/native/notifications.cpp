#include <hyprworks/notifications.hpp>
#include "dbus.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace hyprworks::notifications {
namespace {

using dbus::check;
using dbus::require;
constexpr char interface[] = "org.freedesktop.Notifications";
constexpr char path[] = "/org/freedesktop/Notifications";

std::size_t payload_size(const Notification& notification) {
    std::size_t size = notification.sender.size() + notification.app_name.size() + notification.app_icon.size() + notification.summary.size() + notification.body.size();
    for (const auto& action : notification.actions) size += action.key.size() + action.label.size();
    for (const auto& [key, hint] : notification.hints) {
        size += key.size();
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<std::uint8_t>>) size += value.size();
            else if constexpr (std::is_same_v<T, Image>) size += value.pixels.size();
            else if constexpr (std::is_same_v<T, std::vector<std::string>>) for (const auto& item : value) size += item.size();
            else size += sizeof(value);
        }, hint);
    }
    return size;
}

void read_hints(sd_bus_message* message, Notification& notification, std::size_t& bytes) {
    require(sd_bus_message_enter_container(message, 'a', "{sv}"));
    unsigned count = 0;
    std::size_t binary_bytes = 0;
    while (check(sd_bus_message_enter_container(message, 'e', "sv"))) {
        if (++count > 128) throw std::length_error("notification exceeds 128 hints");
        auto key = dbus::text(message, 's', bytes);
        char type;
        const char* signature = nullptr;
        require(sd_bus_message_peek_type(message, &type, &signature));
        require(sd_bus_message_enter_container(message, 'v', signature));
        std::optional<Hint> hint;
        if (std::strcmp(signature, "s") == 0 || std::strcmp(signature, "o") == 0) hint = dbus::text(message, *signature, bytes);
        else if (std::strcmp(signature, "b") == 0) { int value; require(sd_bus_message_read(message, "b", &value)); hint = value != 0; }
        else if (std::strcmp(signature, "y") == 0) { std::uint8_t value; require(sd_bus_message_read(message, "y", &value)); hint = std::uint64_t(value); }
        else if (std::strcmp(signature, "n") == 0) { std::int16_t value; require(sd_bus_message_read(message, "n", &value)); hint = std::int64_t(value); }
        else if (std::strcmp(signature, "q") == 0) { std::uint16_t value; require(sd_bus_message_read(message, "q", &value)); hint = std::uint64_t(value); }
        else if (std::strcmp(signature, "i") == 0) { std::int32_t value; require(sd_bus_message_read(message, "i", &value)); hint = std::int64_t(value); }
        else if (std::strcmp(signature, "u") == 0) { std::uint32_t value; require(sd_bus_message_read(message, "u", &value)); hint = std::uint64_t(value); }
        else if (std::strcmp(signature, "x") == 0) { std::int64_t value; require(sd_bus_message_read(message, "x", &value)); hint = value; }
        else if (std::strcmp(signature, "t") == 0) { std::uint64_t value; require(sd_bus_message_read(message, "t", &value)); hint = value; }
        else if (std::strcmp(signature, "d") == 0) {
            double value; require(sd_bus_message_read(message, "d", &value));
            if (!std::isfinite(value)) throw std::runtime_error("nonfinite notification hint");
            hint = value;
        } else if (std::strcmp(signature, "as") == 0) {
            hint = dbus::strings(message, bytes);
        } else if (std::strcmp(signature, "(iiibiiay)") == 0 && (key == "image-data" || key == "image_data" || key == "icon_data")) {
            Image image;
            int alpha, bits;
            require(sd_bus_message_enter_container(message, 'r', "iiibiiay"));
            require(sd_bus_message_read(message, "iiibii", &image.width, &image.height, &image.row_stride, &alpha, &bits, &image.channels));
            const void* data; std::size_t size;
            require(sd_bus_message_read_array(message, 'y', &data, &size));
            image.alpha = alpha != 0;
            if (image.width < 1 || image.width > 4096 || image.height < 1 || image.height > 4096 || bits != 8 ||
                image.channels != (image.alpha ? 4 : 3) || image.row_stride < image.width * image.channels || size > 8 * 1024 * 1024 ||
                std::uint64_t(image.height - 1) * std::uint64_t(image.row_stride) + std::uint64_t(image.width * image.channels) > size)
                throw std::runtime_error("invalid notification image dimensions or data");
            binary_bytes += size;
            if (binary_bytes > 8 * 1024 * 1024) throw std::length_error("notification binary hints exceed 8 MiB");
            image.pixels.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
            hint = std::move(image); check(sd_bus_message_exit_container(message));
        } else if (std::strcmp(signature, "ay") == 0) {
            const void* data; std::size_t size;
            require(sd_bus_message_read_array(message, 'y', &data, &size));
            if (size > 8 * 1024 * 1024 || (binary_bytes += size) > 8 * 1024 * 1024) throw std::length_error("notification binary hints exceed 8 MiB");
            std::vector<std::uint8_t> value;
            if (size) value.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
            hint = std::move(value);
        } else require(sd_bus_message_skip(message, signature));
        check(sd_bus_message_exit_container(message)); check(sd_bus_message_exit_container(message));
        if (hint) notification.hints.emplace(std::move(key), std::move(*hint));
    }
    check(sd_bus_message_exit_container(message));
}

}

struct NotificationServer::State {
    sd_bus* bus = nullptr;
    sd_bus_slot *object = nullptr, *ownership = nullptr;
    Options options;
    Status status;
    std::vector<Notification> notifications;
    std::vector<Event> events;
    std::uint32_t last_id = 0;
    std::uint64_t generation = 0;
    bool more = false;
    std::exception_ptr callback_error;
    std::optional<Clock::time_point> deadline;

    void disconnect() noexcept {
        ownership = sd_bus_slot_unref(ownership); object = sd_bus_slot_unref(object);
        bus = sd_bus_close_unref(bus); deadline.reset(); more = false; callback_error = {};
    }

    void ready(std::size_t event_count = 1) const {
        if (status.phase != Phase::ready) throw std::runtime_error("notification server is not ready");
        if (events.size() + event_count > 256) throw std::length_error("notification event queue is full");
    }

    void close(std::size_t index, CloseReason reason) {
        ready();
        const auto id = notifications[index].id;
        events.push_back({EventKind::closed, id, reason, {}, false});
        notifications.erase(notifications.begin() + static_cast<std::ptrdiff_t>(index));
        check(sd_bus_emit_signal(bus, path, interface, "NotificationClosed", "uu", id, static_cast<std::uint32_t>(reason)));
    }

    static int acquired(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (const auto* error = sd_bus_message_get_error(message)) throw std::runtime_error(error->message ? error->message : error->name);
            std::uint32_t reply;
            require(sd_bus_message_read(message, "u", &reply));
            if (reply != 1 && reply != 4) throw std::runtime_error("another notification server owns the name");
            state.status = {Phase::ready, {}}; state.deadline.reset();
        } catch (...) { state.callback_error = std::current_exception(); }
        return 0;
    }

    static int capabilities(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            sd_bus_message* raw = nullptr;
            check(sd_bus_message_new_method_return(message, &raw));
            dbus::Message reply(raw, sd_bus_message_unref);
            check(sd_bus_message_open_container(raw, 'a', "s"));
            for (const auto& capability : state.options.capabilities) check(sd_bus_message_append(raw, "s", capability.c_str()));
            check(sd_bus_message_close_container(raw));
            return sd_bus_send(state.bus, raw, nullptr);
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_FAILED, "%s", exception.what()); }
    }

    static int information(sd_bus_message* message, void* data, sd_bus_error*) noexcept {
        const auto& options = static_cast<State*>(data)->options;
        return sd_bus_reply_method_return(message, "ssss", options.name.c_str(), options.vendor.c_str(), options.version.c_str(), "1.3");
    }

    static int notify(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            state.ready();
            Notification notification;
            std::size_t bytes = 0;
            notification.sender = sd_bus_message_get_sender(message);
            notification.app_name = dbus::text(message, 's', bytes);
            std::uint32_t replaces;
            require(sd_bus_message_read(message, "u", &replaces));
            notification.app_icon = dbus::text(message, 's', bytes);
            notification.summary = dbus::text(message, 's', bytes);
            notification.body = dbus::text(message, 's', bytes);
            require(sd_bus_message_enter_container(message, 'a', "s"));
            while (!check(sd_bus_message_at_end(message, false))) {
                if (notification.actions.size() == 64) throw std::length_error("notification exceeds 64 actions");
                Action action{dbus::text(message, 's', bytes), dbus::text(message, 's', bytes)};
                if (action.key.empty() || std::ranges::find(notification.actions, action.key, &Action::key) != notification.actions.end())
                    throw std::runtime_error("empty or duplicate notification action");
                notification.actions.push_back(std::move(action));
            }
            check(sd_bus_message_exit_container(message));
            read_hints(message, notification, bytes);
            std::int32_t timeout;
            require(sd_bus_message_read(message, "i", &timeout));
            if (timeout < -1) throw std::runtime_error("invalid notification timeout");
            const auto duration = timeout < 0 ? state.options.default_timeout : std::chrono::milliseconds(timeout);
            if (duration && duration->count() > 0) notification.expires_at = Clock::now() + *duration;
            auto found = std::ranges::find(state.notifications, replaces, &Notification::id);
            const bool replacement = replaces && found != state.notifications.end();
            if (!replacement && state.notifications.size() == 128) throw std::length_error("notification limit of 128 reached");
            std::size_t total = payload_size(notification);
            for (const auto& existing : state.notifications) if (!replacement || existing.id != replaces) total += payload_size(existing);
            if (total > 32 * 1024 * 1024) throw std::length_error("notification payload budget of 32 MiB exceeded");
            if (replacement) notification.id = replaces;
            else {
                if (state.last_id == UINT32_MAX) throw std::length_error("notification identifiers exhausted");
                notification.id = ++state.last_id;
            }
            const auto id = notification.id;
            state.events.push_back({EventKind::received, id, CloseReason::undefined, {}, replacement});
            if (replacement) *found = std::move(notification); else state.notifications.push_back(std::move(notification));
            return sd_bus_reply_method_return(message, "u", id);
        } catch (const std::length_error& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_LIMITS_EXCEEDED, "%s", exception.what()); }
        catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s", exception.what()); }
    }

    static int close_request(sd_bus_message* message, void* data, sd_bus_error* error) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            std::uint32_t id;
            require(sd_bus_message_read(message, "u", &id));
            const auto found = std::ranges::find(state.notifications, id, &Notification::id);
            if (found == state.notifications.end()) throw std::runtime_error("unknown notification");
            state.close(static_cast<std::size_t>(found - state.notifications.begin()), CloseReason::requested);
            return sd_bus_reply_method_return(message, "");
        } catch (const std::exception& exception) { return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s", exception.what()); }
    }
};

NotificationServer::NotificationServer() : state_(std::make_unique<State>()) {}
NotificationServer::~NotificationServer() { stop(); }

std::expected<void, std::string> NotificationServer::start(const Options& options, const std::string& address) {
    auto& state = *state_;
    if (state.bus) return std::unexpected("notification server is already running");
    stop();
    try {
        for (const auto* text : {&options.name, &options.vendor, &options.version})
            if (text->size() > 128 || text->contains('\0')) throw std::runtime_error("invalid notification server information");
        if ((options.default_timeout && (options.default_timeout->count() < 0 || options.default_timeout->count() > INT32_MAX)) ||
            options.capabilities.size() > 64)
            throw std::runtime_error("invalid notification defaults or capabilities");
        for (const auto& capability : options.capabilities)
            if (capability.empty() || capability.size() > 128 || capability.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
                throw std::runtime_error("invalid notification capability");
        if (std::ranges::find(options.capabilities, "icon-static") != options.capabilities.end() && std::ranges::find(options.capabilities, "icon-multi") != options.capabilities.end())
            throw std::runtime_error("icon-static and icon-multi are mutually exclusive");
        state.options = options;
        dbus::open(state.bus, address);
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCapabilities", "", "as", State::capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetServerInformation", "", "ssss", State::information, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", State::notify, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CloseNotification", "u", "", State::close_request, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("NotificationClosed", "uu", 0), SD_BUS_SIGNAL("ActionInvoked", "us", 0), SD_BUS_SIGNAL("ActivationToken", "us", 0),
            SD_BUS_VTABLE_END
        };
        check(sd_bus_add_object_vtable(state.bus, &state.object, path, interface, vtable, &state));
        state.status = {Phase::connecting, {}}; state.deadline = Clock::now() + std::chrono::seconds(10);
        check(sd_bus_request_name_async(state.bus, &state.ownership, interface, 0, State::acquired, &state));
        return {};
    } catch (const std::exception& error) {
        state.disconnect(); state.status = {Phase::failed, error.what()}; return std::unexpected(state.status.error);
    }
}

void NotificationServer::stop() noexcept {
    state_->disconnect(); state_->status = {}; state_->notifications.clear(); state_->events.clear(); ++state_->generation;
}

std::expected<void, std::string> NotificationServer::close(std::uint32_t id, CloseReason reason) {
    try {
        auto& state = *state_;
        const auto found = std::ranges::find(state.notifications, id, &Notification::id);
        if (found == state.notifications.end() || static_cast<unsigned>(reason) < 1 || static_cast<unsigned>(reason) > 4)
            throw std::runtime_error("unknown notification or close reason");
        state.close(static_cast<std::size_t>(found - state.notifications.begin()), reason);
        ++state.generation; return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<void, std::string> NotificationServer::invoke(std::uint32_t id, const std::string& action, const std::string& activation_token) {
    try {
        auto& state = *state_;
        state.ready(2);
        const auto found = std::ranges::find(state.notifications, id, &Notification::id);
        if (found == state.notifications.end() || std::ranges::find(found->actions, action, &Action::key) == found->actions.end())
            throw std::runtime_error("unknown notification action");
        if (activation_token.size() > 16384 || activation_token.contains('\0')) throw std::runtime_error("invalid activation token");
        const auto resident_hint = found->hints.find("resident");
        const bool resident = resident_hint != found->hints.end() && std::get_if<bool>(&resident_hint->second) && std::get<bool>(resident_hint->second);
        if (!activation_token.empty()) check(sd_bus_emit_signal(state.bus, path, interface, "ActivationToken", "us", id, activation_token.c_str()));
        check(sd_bus_emit_signal(state.bus, path, interface, "ActionInvoked", "us", id, action.c_str()));
        state.events.push_back({EventKind::action, id, CloseReason::undefined, action, false});
        if (!resident) state.close(static_cast<std::size_t>(found - state.notifications.begin()), CloseReason::dismissed);
        ++state.generation; return {};
    } catch (const std::exception& error) { return std::unexpected(error.what()); }
}

WaitSet NotificationServer::prepare_wait() {
    auto& state = *state_;
    WaitSet wait; wait.generation = ++state.generation;
    if (!state.bus) return wait;
    wait.deadline = state.deadline;
    dbus::prepare(state.bus, wait.fd, wait.deadline);
    for (const auto& notification : state.notifications)
        if (notification.expires_at && (!wait.deadline || *notification.expires_at < *wait.deadline)) wait.deadline = notification.expires_at;
    if (state.more || !state.events.empty()) wait.deadline = Clock::time_point::min();
    return wait;
}

Update NotificationServer::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fd.fd != (state.bus ? sd_bus_get_fd(state.bus) : -1))
        return {};
    ++state.generation;
    const auto before = state.status;
    if (!state.bus) return {};
    try {
        state.more = dbus::process(state.bus, state.callback_error);
        const auto now = Clock::now();
        if (state.deadline && now >= *state.deadline) throw std::runtime_error("notification name acquisition timed out");
        if (state.status.phase == Phase::ready) {
            for (std::size_t i = 0; i < state.notifications.size() && state.events.size() < 256;)
                if (state.notifications[i].expires_at && now >= *state.notifications[i].expires_at) state.close(i, CloseReason::expired);
                else ++i;
        }
    } catch (const std::exception& error) { state.disconnect(); state.status = {Phase::failed, error.what()}; }
    return {before != state.status, std::exchange(state.events, {})};
}

const Status& NotificationServer::status() const noexcept { return state_->status; }
const std::vector<Notification>& NotificationServer::notifications() const noexcept { return state_->notifications; }

}
