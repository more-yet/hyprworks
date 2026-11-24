#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <hyprworks/dbus.hpp>
#include <fcntl.h>

using hyprworks::dbus::Body;
using hyprworks::dbus::Value;
using hyprworks::dbus::Connection;
using hyprworks::dbus::Event;
using hyprworks::dbus::EventKind;

static std::vector<Event> messages;
static void receive(Connection& bus) {
    auto wait = bus.prepare_wait();
    int timeout = 100;
    if (wait.deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *wait.deadline - hyprworks::dbus::Clock::now()).count();
        timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, timeout));
    }
    assert(poll(&wait.fd, 1, timeout) >= 0);
    auto update = bus.dispatch(wait);
    assert(bus.status().phase != hyprworks::dbus::Phase::failed);
    for (auto& event : update.events) messages.push_back(std::move(event));
    assert(messages.size() < 64);
}
static Body call(Connection& bus, const std::string& interface, const std::string& member, Body arguments,
                 const std::string& path = "/org/freedesktop/portal/desktop") {
    const auto id = bus.call({"org.freedesktop.portal.Desktop", path, interface, member, std::move(arguments), std::chrono::seconds(15)});
    assert(id);
    const auto until = hyprworks::dbus::Clock::now() + std::chrono::seconds(16);
    for (;;) {
        const auto event = std::ranges::find_if(messages, [&](const auto& message) { return message.kind == EventKind::reply && message.id == *id; });
        if (event != messages.end()) {
            if (!event->error.empty()) std::cerr << event->error << '\n';
            assert(event->error.empty());
            auto result = std::move(event->arguments); messages.erase(event); return result;
        }
        assert(hyprworks::dbus::Clock::now() < until);
        receive(bus);
    }
}
static const Value& field(const Value& dictionary, const std::string& key) {
    for (const auto& entry : std::get<Body>(dictionary.data)) {
        const auto& pair = std::get<Body>(entry.data);
        if (std::get<std::string>(pair[0].data) == key) return std::get<Body>(pair[1].data).front();
    }
    std::abort();
}
static Value options(std::initializer_list<std::pair<std::string, Value>> fields) {
    Body entries;
    for (const auto& [name, value] : fields)
        entries.push_back({"{sv}", Body{{"s", name}, {"v", Body{value}}}});
    return {"a{sv}", std::move(entries)};
}
static Value request(Connection& bus, const std::string& member, Body arguments) {
    const auto reply = call(bus, "org.freedesktop.portal.ScreenCast", member, std::move(arguments));
    const auto path = std::get<std::string>(reply[0].data);
    const auto until = hyprworks::dbus::Clock::now() + std::chrono::seconds(16);
    for (;;) {
        const auto event = std::ranges::find_if(messages, [&](const auto& message) {
            return message.kind == EventKind::signal && message.path == path && message.member == "Response";
        });
        if (event != messages.end()) {
            assert(std::get<std::uint64_t>(event->arguments[0].data) == 0);
            auto result = std::move(event->arguments[1]); messages.erase(event); return result;
        }
        assert(hyprworks::dbus::Clock::now() < until);
        receive(bus);
    }
}

struct Consumer {
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    unsigned frames = 0;
    bool failed = false;
};

int main(int argc, char** argv) {
    assert(argc == 2);
    Connection portal;
    hyprworks::dbus::FileDescriptor remote;
    std::string node = argv[1], session;
    if (node == "--portal" || node == "--portal-monitor") {
        const std::uint64_t types = node == "--portal-monitor" ? 1 : 2;
        assert(portal.start(false));
        assert(portal.subscribe("type='signal',interface='org.freedesktop.portal.Request',member='Response'"));
        const auto created = request(portal, "CreateSession", {options({
            {"handle_token", {"s", std::string("create")}}, {"session_handle_token", {"s", std::string("capture")}}})});
        session = std::get<std::string>(field(created, "session_handle").data);
        request(portal, "SelectSources", {{"o", session}, options({
            {"types", {"u", types}}, {"handle_token", {"s", std::string("select")}}})});
        const auto started = request(portal, "Start", {{"o", session}, {"s", std::string{}},
            options({{"handle_token", {"s", std::string("start")}}})});
        const auto& streams = std::get<Body>(field(started, "streams").data);
        node = std::to_string(std::get<std::uint64_t>(std::get<Body>(streams.front().data).front().data));
        const auto result = call(portal, "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote", {{"o", session}, {"a{sv}", Body{}}});
        remote = std::get<hyprworks::dbus::FileDescriptor>(result.front().data);
    }
    pw_init(nullptr, nullptr);
    Consumer consumer;
    consumer.loop = pw_main_loop_new(nullptr);
    assert(consumer.loop);
    auto* loop = pw_main_loop_get_loop(consumer.loop);
    auto* context = pw_context_new(loop, nullptr, 0);
    assert(context);
    auto* core = remote.get() < 0 ? pw_context_connect(context, nullptr, 0) :
        pw_context_connect_fd(context, fcntl(remote.get(), F_DUPFD_CLOEXEC, 3), nullptr, 0);
    assert(core);
    consumer.stream = pw_stream_new(core, "Hyprworks capture lifetime check",
        pw_properties_new(PW_KEY_TARGET_OBJECT, node.c_str(), PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    assert(consumer.stream);
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char* error) {
        auto& c = *static_cast<Consumer*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            std::cerr << (error ? error : "stream failed") << '\n';
            c.failed = true; pw_main_loop_quit(c.loop);
        }
    };
    events.param_changed = [](void* data, std::uint32_t id, const spa_pod* param) {
        if (id != SPA_PARAM_Format || !param) return;
        auto& c = *static_cast<Consumer*>(data);
        spa_video_info_raw format{};
        assert(spa_format_video_raw_parse(param, &format) >= 0);
        assert(format.format == SPA_VIDEO_FORMAT_BGRA && format.size.width && format.size.height);
        std::uint8_t bytes[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
        const spa_pod* params[] = {static_cast<spa_pod*>(spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<int>(format.size.width * format.size.height * 4)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int>(format.size.width * 4)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)))};
        assert(pw_stream_update_params(c.stream, params, 1) == 0);
    };
    events.process = [](void* data) {
        auto& c = *static_cast<Consumer*>(data);
        auto* buffer = pw_stream_dequeue_buffer(c.stream);
        if (!buffer) return;
        assert(buffer->buffer->n_datas);
        auto& plane = buffer->buffer->datas[0];
        if (plane.chunk && plane.chunk->size && !(plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
            assert(plane.data && plane.chunk->size <= plane.maxsize && plane.chunk->offset <= plane.maxsize - plane.chunk->size);
            const auto* bytes = static_cast<const unsigned char*>(plane.data) + plane.chunk->offset;
            bool nonzero = false;
            for (std::uint32_t i = 0; i < plane.chunk->size; ++i) nonzero |= bytes[i] != 0;
            assert(nonzero);
            ++c.frames;
        }
        assert(pw_stream_queue_buffer(c.stream, buffer) == 0);
        if (c.frames >= 4) pw_main_loop_quit(c.loop);
    };
    pw_stream_add_listener(consumer.stream, &consumer.listener, &events, &consumer);
    std::uint8_t bytes[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    const spa_rectangle initial{320, 240}, minimum{1, 1}, maximum{4096, 4096};
    const spa_fraction rate{30, 1}, min_rate{1, 1}, max_rate{60, 1};
    const spa_pod* params[] = {static_cast<spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&initial, &minimum, &maximum),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &min_rate, &max_rate)))};
    assert(pw_stream_connect(consumer.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) == 0);
    auto* timer = pw_loop_add_timer(loop, [](void* data, std::uint64_t) {
        auto& c = *static_cast<Consumer*>(data);
        c.failed = true; pw_main_loop_quit(c.loop);
    }, &consumer);
    timespec timeout{8, 0};
    pw_loop_update_timer(loop, timer, &timeout, nullptr, false);
    pw_main_loop_run(consumer.loop);
    spa_hook_remove(&consumer.listener);
    pw_stream_destroy(consumer.stream);
    pw_loop_destroy_source(loop, timer);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(consumer.loop);
    pw_deinit();
    if (!session.empty()) call(portal, "org.freedesktop.portal.Session", "Close", {}, session);
    if (consumer.failed || consumer.frames < 4) return 1;
    std::cout << "PipeWire: four mapped frames received and buffers returned\n";
}
