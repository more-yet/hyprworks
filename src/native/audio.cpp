#include <hyprworks/audio.hpp>
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>

namespace hyprworks::audio {
namespace {

// libpulse owns the descriptors and callback data; these records own only event registrations.
struct PulseLoop {
    struct Event {
        enum class Kind { unused, io, timer, defer } kind = Kind::unused;
        bool dead = false, enabled = false;
        int fd = -1;
        pa_io_event_flags_t flags = PA_IO_EVENT_NULL;
        timeval time{};
        Clock::time_point deadline{};
        void* userdata = nullptr;
        pa_io_event_cb_t io = nullptr;
        pa_time_event_cb_t timer = nullptr;
        pa_defer_event_cb_t defer = nullptr;
        pa_io_event_destroy_cb_t destroy_io = nullptr;
        pa_time_event_destroy_cb_t destroy_timer = nullptr;
        pa_defer_event_destroy_cb_t destroy_defer = nullptr;
    };
    std::array<Event, 64> events{};
    pa_mainloop_api api{};
    bool quit = false;

    Event* allocate(Event::Kind kind, void* userdata) noexcept {
        for (auto& event : events) {
            if (event.kind != Event::Kind::unused) continue;
            event.kind = kind;
            event.userdata = userdata;
            event.enabled = true;
            return &event;
        }
        // One context, one query and one mutation must fit; libpulse requires non-null registrations.
        assert(false && "audio event registration capacity exhausted");
        std::abort();
    }

    static void restart(pa_time_event* source, const timeval* time) noexcept {
        auto& event = *reinterpret_cast<Event*>(source);
        event.enabled = time != nullptr;
        if (!time) return;
        event.time = *time;
        const auto wall = std::chrono::seconds(time->tv_sec) + std::chrono::microseconds(time->tv_usec);
        event.deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(wall - std::chrono::system_clock::now().time_since_epoch());
    }

    PulseLoop() {
        api.userdata = this;
        api.io_new = [](pa_mainloop_api* api, int fd, pa_io_event_flags_t flags, pa_io_event_cb_t callback, void* data) -> pa_io_event* {
            auto* event = static_cast<PulseLoop*>(api->userdata)->allocate(Event::Kind::io, data);
            event->fd = fd; event->flags = flags; event->io = callback;
            return reinterpret_cast<pa_io_event*>(event);
        };
        api.io_enable = [](pa_io_event* event, pa_io_event_flags_t flags) { reinterpret_cast<Event*>(event)->flags = flags; };
        api.io_free = [](pa_io_event* event) { reinterpret_cast<Event*>(event)->dead = true; };
        api.io_set_destroy = [](pa_io_event* event, pa_io_event_destroy_cb_t callback) { reinterpret_cast<Event*>(event)->destroy_io = callback; };
        api.time_new = [](pa_mainloop_api* api, const timeval* time, pa_time_event_cb_t callback, void* data) -> pa_time_event* {
            auto* event = static_cast<PulseLoop*>(api->userdata)->allocate(Event::Kind::timer, data);
            event->timer = callback;
            auto* source = reinterpret_cast<pa_time_event*>(event);
            restart(source, time);
            return source;
        };
        api.time_restart = restart;
        api.time_free = [](pa_time_event* event) { reinterpret_cast<Event*>(event)->dead = true; };
        api.time_set_destroy = [](pa_time_event* event, pa_time_event_destroy_cb_t callback) { reinterpret_cast<Event*>(event)->destroy_timer = callback; };
        api.defer_new = [](pa_mainloop_api* api, pa_defer_event_cb_t callback, void* data) -> pa_defer_event* {
            auto* event = static_cast<PulseLoop*>(api->userdata)->allocate(Event::Kind::defer, data);
            event->defer = callback;
            return reinterpret_cast<pa_defer_event*>(event);
        };
        api.defer_enable = [](pa_defer_event* event, int enabled) { reinterpret_cast<Event*>(event)->enabled = enabled != 0; };
        api.defer_free = [](pa_defer_event* event) { reinterpret_cast<Event*>(event)->dead = true; };
        api.defer_set_destroy = [](pa_defer_event* event, pa_defer_event_destroy_cb_t callback) { reinterpret_cast<Event*>(event)->destroy_defer = callback; };
        api.quit = [](pa_mainloop_api* api, int) { static_cast<PulseLoop*>(api->userdata)->quit = true; };
    }

    void collect(bool all = false) noexcept {
        for (auto& event : events) {
            if (event.kind == Event::Kind::unused || (!all && !event.dead)) continue;
            if (event.destroy_io) event.destroy_io(&api, reinterpret_cast<pa_io_event*>(&event), event.userdata);
            if (event.destroy_timer) event.destroy_timer(&api, reinterpret_cast<pa_time_event*>(&event), event.userdata);
            if (event.destroy_defer) event.destroy_defer(&api, reinterpret_cast<pa_defer_event*>(&event), event.userdata);
            event = {};
        }
    }

    void prepare(WaitSet& wait) {
        collect();
        // Fixed slots keep readiness associated with its source even when other callbacks remove events.
        wait.fds.resize(events.size(), {-1, 0, 0});
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];
            if (event.kind == Event::Kind::io && event.flags != PA_IO_EVENT_NULL) {
                wait.fds[i] = {event.fd, static_cast<short>((event.flags & PA_IO_EVENT_INPUT ? POLLIN : 0) |
                                                        (event.flags & PA_IO_EVENT_OUTPUT ? POLLOUT : 0)), 0};
            } else if (event.enabled && (event.kind == Event::Kind::timer || event.kind == Event::Kind::defer)) {
                const auto time = event.kind == Event::Kind::defer ? Clock::time_point::min() : event.deadline;
                if (!wait.deadline || time < *wait.deadline) wait.deadline = time;
            }
        }
        if (quit) wait.deadline = Clock::time_point::min();
    }

    void dispatch(const WaitSet& ready) {
        std::array<bool, 64> due{};
        const auto now = Clock::now();
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];
            due[i] = event.enabled && (event.kind == Event::Kind::defer ||
                     (event.kind == Event::Kind::timer && event.deadline <= now));
        }
        for (std::size_t i = 0; i < events.size(); ++i) {
            auto& event = events[i];
            if (event.dead || !event.enabled) continue;
            if (event.kind == Event::Kind::io && ready.fds[i].fd == event.fd && ready.fds[i].revents) {
                const auto revents = ready.fds[i].revents;
                const auto flags = static_cast<pa_io_event_flags_t>((revents & POLLIN ? PA_IO_EVENT_INPUT : 0) |
                    (revents & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) | (revents & POLLHUP ? PA_IO_EVENT_HANGUP : 0) |
                    (revents & (POLLERR | POLLNVAL) ? PA_IO_EVENT_ERROR : 0));
                event.io(&api, reinterpret_cast<pa_io_event*>(&event), event.fd, flags, event.userdata);
            } else if (due[i] && event.kind == Event::Kind::timer && event.deadline <= now) {
                event.enabled = false;
                const auto time = event.time;
                event.timer(&api, reinterpret_cast<pa_time_event*>(&event), &time, event.userdata);
            } else if (due[i] && event.kind == Event::Kind::defer)
                event.defer(&api, reinterpret_cast<pa_defer_event*>(&event), event.userdata);
        }
        if (quit) throw std::runtime_error("audio event loop stopped");
    }
};

void release(pa_operation*& operation) noexcept {
    if (!operation) return;
    pa_operation_cancel(operation);
    pa_operation_unref(std::exchange(operation, nullptr));
}

}

struct AudioConnection::State {
    PulseLoop loop;
    pa_context* context = nullptr;
    pa_operation* query = nullptr;
    pa_operation* command = nullptr;
    Status status;
    std::optional<Snapshot> snapshot;
    Snapshot pending;
    enum class Step { subscribe, server, outputs, inputs } step = Step::subscribe;
    bool query_done = false, subscribed = false, dirty = false;
    std::size_t bytes = 0;
    std::uint64_t generation = 0, last_request = 0;
    std::optional<RequestResult> result;
    std::optional<Clock::time_point> deadline, refresh_at, command_deadline;
    std::exception_ptr callback_error;
    WaitSet prepared;

    void disconnect() noexcept {
        release(query);
        release(command);
        if (context) {
            pa_context_set_subscribe_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(std::exchange(context, nullptr));
        }
        loop.collect(true);
        loop.quit = false;
        deadline.reset(); refresh_at.reset(); command_deadline.reset();
        query_done = subscribed = dirty = false;
        pending = {};
        callback_error = {};
    }

    void invalidate() noexcept {
        dirty = true;
        if (!refresh_at) refresh_at = Clock::now() + std::chrono::milliseconds(16);
    }

    std::string text(const char* value) {
        if (!value) return {};
        const auto length = ::strnlen(value, 16385);
        if (length > 16384 || bytes + length > 1024 * 1024)
            throw std::runtime_error("audio strings exceed 16 KiB per field or 1 MiB per snapshot");
        bytes += length;
        return {value, length};
    }

    template<typename Info>
    void device(const Info& info, std::vector<Device>& devices) {
        if (pending.inputs.size() + pending.outputs.size() >= 128)
            throw std::runtime_error("audio exceeds 128 devices");
        if (!info.name || !*info.name || info.index == PA_INVALID_INDEX || !pa_channel_map_valid(&info.channel_map) ||
            !pa_cvolume_valid(&info.volume) || info.channel_map.channels != info.volume.channels)
            throw std::runtime_error("invalid audio device or channel map");
        Device device;
        device.name = text(info.name); device.description = text(info.description);
        device.icon_name = text(pa_proplist_gets(info.proplist, PA_PROP_DEVICE_ICON_NAME));
        device.index = info.index;
        switch (static_cast<int>(info.state)) {
            case PA_SINK_RUNNING: device.state = DeviceState::running; break;
            case PA_SINK_IDLE: device.state = DeviceState::idle; break;
            case PA_SINK_SUSPENDED: device.state = DeviceState::suspended; break;
            default: break;
        }
        device.mute = info.mute != 0;
        if constexpr (requires { info.monitor_of_sink; }) device.monitor = info.monitor_of_sink != PA_INVALID_INDEX;
        for (unsigned i = 0; i < info.volume.channels; ++i)
            device.channels.push_back({text(pa_channel_position_to_string(info.channel_map.map[i])),
                                       static_cast<double>(info.volume.values[i]) / PA_VOLUME_NORM});
        devices.push_back(std::move(device));
    }

    static void changed(pa_context*, pa_subscription_event_type_t, std::uint32_t, void* data) noexcept {
        static_cast<State*>(data)->invalidate();
    }

    static void subscribed_reply(pa_context* context, int success, void* data) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (!success) throw std::runtime_error(pa_strerror(pa_context_errno(context)));
            state.query_done = true;
        } catch (...) { state.callback_error = std::current_exception(); }
    }

    static void server_reply(pa_context* context, const pa_server_info* info, void* data) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            if (!info) throw std::runtime_error(pa_strerror(pa_context_errno(context)));
            state.pending.default_output = state.text(info->default_sink_name);
            state.pending.default_input = state.text(info->default_source_name);
            state.query_done = true;
        } catch (...) { state.callback_error = std::current_exception(); }
    }

    template<typename Info>
    static void devices_reply(pa_context* context, const Info* info, int eol, void* data) noexcept {
        auto& state = *static_cast<State*>(data);
        if (state.callback_error) return;
        try {
            if (eol < 0) throw std::runtime_error(pa_strerror(pa_context_errno(context)));
            if (eol) state.query_done = true;
            else if (info) state.device(*info, state.step == Step::outputs ? state.pending.outputs : state.pending.inputs);
            else throw std::runtime_error("incomplete audio device reply");
        } catch (...) { state.callback_error = std::current_exception(); }
    }

    void send_query() {
        query_done = false;
        switch (step) {
            case Step::subscribe:
                query = pa_context_subscribe(context, static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SERVER |
                    PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE), subscribed_reply, this);
                break;
            case Step::server: query = pa_context_get_server_info(context, server_reply, this); break;
            case Step::outputs: query = pa_context_get_sink_info_list(context, devices_reply<pa_sink_info>, this); break;
            case Step::inputs: query = pa_context_get_source_info_list(context, devices_reply<pa_source_info>, this); break;
        }
        if (!query) throw std::runtime_error(pa_strerror(pa_context_errno(context)));
    }

    void publish() {
        for (auto* devices : {&pending.outputs, &pending.inputs}) {
            std::ranges::sort(*devices, {}, &Device::name);
            for (std::size_t i = 1; i < devices->size(); ++i)
                if ((*devices)[i - 1].name == (*devices)[i].name) throw std::runtime_error("duplicate audio device name");
        }
        pending.revision = snapshot ? snapshot->revision : 0;
        if (!snapshot || pending != *snapshot) {
            if (pending.revision == UINT64_MAX) throw std::runtime_error("audio snapshot revision exhausted");
            ++pending.revision;
            snapshot = std::move(pending);
        }
        pending = {};
        status = {Phase::ready, {}};
        deadline.reset();
    }

    std::expected<const Device*, std::string> target(Direction direction, const std::string& name) const {
        if (status.phase != Phase::ready && !(status.phase == Phase::synchronizing && snapshot))
            return std::unexpected("audio connection is not ready");
        if (command || result) return std::unexpected("an audio request is already outstanding");
        if (last_request == UINT64_MAX) return std::unexpected("audio request identifiers exhausted");
        if (direction != Direction::output && direction != Direction::input) return std::unexpected("invalid audio direction");
        const auto& devices = direction == Direction::output ? snapshot->outputs : snapshot->inputs;
        const auto found = std::ranges::find(devices, name, &Device::name);
        if (found == devices.end()) return std::unexpected("audio device is not in the published snapshot");
        return &*found;
    }

    static void command_reply(pa_context* context, int success, void* data) noexcept {
        auto& state = *static_cast<State*>(data);
        try {
            state.result = RequestResult{state.last_request, success ? Outcome::acknowledged : Outcome::rejected,
                                         success != 0, success ? "" : pa_strerror(pa_context_errno(context))};
            state.invalidate();
        } catch (...) { state.callback_error = std::current_exception(); }
    }

    std::expected<std::uint64_t, std::string> sent(pa_operation* operation) {
        if (!operation) return std::unexpected(pa_strerror(pa_context_errno(context)));
        command = operation;
        command_deadline = Clock::now() + std::chrono::seconds(2);
        ++generation;
        return ++last_request;
    }
};

AudioConnection::AudioConnection() : state_(std::make_unique<State>()) {}
AudioConnection::~AudioConnection() { stop(); }

std::expected<void, std::string> AudioConnection::start(const std::string& server) {
    auto& state = *state_;
    if (state.context) return std::unexpected("audio connection is already running");
    if (server.size() > 16384 || server.contains('\0')) return std::unexpected("invalid audio server address");
    stop();
    state.snapshot.reset();
    try {
        state.context = pa_context_new(&state.loop.api, "hyprworks");
        if (!state.context) throw std::runtime_error("cannot create audio context");
        pa_context_set_subscribe_callback(state.context, State::changed, &state);
        if (pa_context_connect(state.context, server.empty() ? nullptr : server.c_str(), PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
            throw std::runtime_error(pa_strerror(pa_context_errno(state.context)));
        state.status = {Phase::connecting, {}};
        state.deadline = Clock::now() + std::chrono::seconds(10);
        return {};
    } catch (const std::exception& error) {
        state.disconnect();
        state.status = {Phase::failed, error.what()};
        return std::unexpected(state.status.error);
    }
}

std::optional<RequestResult> AudioConnection::stop() noexcept {
    auto& state = *state_;
    auto result = std::exchange(state.result, {});
    if (state.command && !result) result = RequestResult{state.last_request, Outcome::cancelled, true, {}};
    ++state.generation;
    state.disconnect();
    state.status = {};
    return result;
}

std::expected<void, std::string> AudioConnection::refresh() {
    if (!state_->context) return std::unexpected("audio connection is not running");
    state_->invalidate();
    ++state_->generation;
    return {};
}

std::expected<std::uint64_t, std::string> AudioConnection::set_volume(Direction direction, const std::string& name, std::span<const double> volumes) {
    auto& state = *state_;
    const auto device = state.target(direction, name);
    if (!device) return std::unexpected(device.error());
    if (volumes.size() != (*device)->channels.size()) return std::unexpected("volume count must match the device channels");
    pa_cvolume volume{};
    volume.channels = static_cast<std::uint8_t>(volumes.size());
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (!std::isfinite(volumes[i]) || volumes[i] < 0 || volumes[i] > static_cast<double>(PA_VOLUME_MAX) / PA_VOLUME_NORM)
            return std::unexpected("volume is outside the PulseAudio range");
        volume.values[i] = static_cast<pa_volume_t>(std::round(volumes[i] * PA_VOLUME_NORM));
    }
    return state.sent(direction == Direction::output ?
        pa_context_set_sink_volume_by_name(state.context, name.c_str(), &volume, State::command_reply, &state) :
        pa_context_set_source_volume_by_name(state.context, name.c_str(), &volume, State::command_reply, &state));
}

std::expected<std::uint64_t, std::string> AudioConnection::set_mute(Direction direction, const std::string& name, bool mute) {
    auto& state = *state_;
    if (const auto device = state.target(direction, name); !device) return std::unexpected(device.error());
    return state.sent(direction == Direction::output ?
        pa_context_set_sink_mute_by_name(state.context, name.c_str(), mute, State::command_reply, &state) :
        pa_context_set_source_mute_by_name(state.context, name.c_str(), mute, State::command_reply, &state));
}

std::expected<std::uint64_t, std::string> AudioConnection::set_default(Direction direction, const std::string& name) {
    auto& state = *state_;
    if (const auto device = state.target(direction, name); !device) return std::unexpected(device.error());
    return state.sent(direction == Direction::output ?
        pa_context_set_default_sink(state.context, name.c_str(), State::command_reply, &state) :
        pa_context_set_default_source(state.context, name.c_str(), State::command_reply, &state));
}

WaitSet AudioConnection::prepare_wait() {
    auto& state = *state_;
    WaitSet wait;
    wait.generation = ++state.generation;
    if (state.context) {
        wait.deadline = state.deadline;
        state.loop.prepare(wait);
        for (const auto& time : {state.command_deadline, state.dirty && state.subscribed && !state.query ? state.refresh_at : std::nullopt})
            if (time && (!wait.deadline || *time < *wait.deadline)) wait.deadline = time;
    }
    state.prepared = wait;
    return wait;
}

Update AudioConnection::dispatch(const WaitSet& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fds.size() != state.prepared.fds.size())
        throw std::logic_error("dispatch requires the current audio wait set");
    for (std::size_t i = 0; i < ready.fds.size(); ++i)
        if (ready.fds[i].fd != state.prepared.fds[i].fd || ready.fds[i].events != state.prepared.fds[i].events)
            throw std::logic_error("audio wait descriptors were modified");
    ++state.generation;
    const auto before = state.status;
    const auto revision = state.snapshot ? state.snapshot->revision : 0;
    if (!state.context) return {};
    try {
        state.loop.dispatch(ready);
        if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, {}));
        const auto phase = pa_context_get_state(state.context);
        if (phase == PA_CONTEXT_FAILED || phase == PA_CONTEXT_TERMINATED)
            throw std::runtime_error(pa_strerror(pa_context_errno(state.context)));
        const auto now = Clock::now();
        if (state.command && !state.result && now >= *state.command_deadline)
            throw std::runtime_error("audio request timed out");
        if (state.deadline && now >= *state.deadline) throw std::runtime_error("audio synchronization timed out");
        if (phase == PA_CONTEXT_READY && state.status.phase == Phase::connecting) {
            state.status = {Phase::synchronizing, {}};
            state.step = State::Step::subscribe;
            state.send_query();
        }
        if (state.query_done) {
            release(state.query);
            state.query_done = false;
            switch (state.step) {
                case State::Step::subscribe:
                    state.subscribed = true;
                    state.invalidate();
                    state.refresh_at = now;
                    break;
                case State::Step::server: state.step = State::Step::outputs; state.send_query(); break;
                case State::Step::outputs: state.step = State::Step::inputs; state.send_query(); break;
                case State::Step::inputs: state.publish(); break;
            }
        }
        if (state.dirty && state.subscribed && !state.query && now >= *state.refresh_at) {
            state.dirty = false;
            state.refresh_at.reset();
            state.pending = {};
            state.bytes = 0;
            state.step = State::Step::server;
            state.status = {Phase::synchronizing, {}};
            state.deadline = now + std::chrono::seconds(10);
            state.send_query();
        }
    } catch (const std::exception& error) {
        if (state.command && !state.result) state.result = RequestResult{state.last_request, Outcome::failed, true, error.what()};
        state.disconnect();
        state.status = {Phase::failed, error.what()};
    }
    if (state.result) {
        release(state.command);
        state.command_deadline.reset();
    }
    return {before != state.status, revision != (state.snapshot ? state.snapshot->revision : 0), std::exchange(state.result, {})};
}

const Status& AudioConnection::status() const noexcept { return state_->status; }
const std::optional<Snapshot>& AudioConnection::snapshot() const noexcept { return state_->snapshot; }

}
