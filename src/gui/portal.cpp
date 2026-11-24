#include "../runtime/invocation.hpp"
#include "gui.hpp"
#include "portal.hpp"
#include <spa/param/video/format-utils.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <sys/random.h>

namespace hyprworks::lua {
namespace {

using dbus::Value;
using dbus::Body;
using Clock = dbus::Clock;
constexpr char backend[] = "org.freedesktop.impl.portal.desktop.hyprworks";
constexpr char frontend_name[] = "org.freedesktop.portal.Desktop";
constexpr char ui_name[] = "org.hyprworks.PortalUI";
constexpr char path[] = "/org/freedesktop/portal/desktop";
constexpr char screencast[] = "org.freedesktop.impl.portal.ScreenCast";
constexpr char session_interface[] = "org.freedesktop.impl.portal.Session";
constexpr char request_interface[] = "org.freedesktop.impl.portal.Request";
constexpr char control[] = "org.hyprworks.Portal";
constexpr auto request_timeout = std::chrono::minutes(3);
constexpr std::size_t stream_limit = 256 * 1024 * 1024;

Value number(std::uint64_t value, const char* signature = "u") { return {signature, value}; }
Value text(std::string value, const char* signature = "s") { return {signature, std::move(value)}; }
Value dictionary(std::initializer_list<std::pair<std::string, Value>> fields) {
    Body entries;
    for (const auto& [key, value] : fields)
        entries.push_back({"{sv}", Body{text(key), {"v", Body{value}}}});
    return {"a{sv}", std::move(entries)};
}
const Body& items(const Value& value) { return std::get<Body>(value.data); }
const std::string& string(const Value& value) { return std::get<std::string>(value.data); }
std::uint64_t integer(const Value& value) { return std::get<std::uint64_t>(value.data); }
template<class T>
T require(std::expected<T, std::string> result) {
    if (!result) throw std::runtime_error(result.error());
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}
Body response(std::uint32_t code) { return {number(code), dictionary({})}; }

const pw_core_events core_events = [] {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.error = [](void* data, std::uint32_t id, int, int result, const char*) {
        if (id == PW_ID_CORE || result == -EPIPE) static_cast<Portal*>(data)->pipewire_failed = true;
    };
    return events;
}();

const pw_stream_events stream_events = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char*) {
        auto& stream = *static_cast<PortalStream*>(data);
        stream.state = state;
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) stream.failed = true;
        if (state == PW_STREAM_STATE_STREAMING) stream.next_process = Clock::now();
        else stream.next_process.reset();
    };
    events.param_changed = [](void* data, std::uint32_t id, const spa_pod* parameter) {
        if (id != SPA_PARAM_Format || !parameter) return;
        auto& stream = *static_cast<PortalStream*>(data);
        spa_video_info_raw format{};
        if (spa_format_video_raw_parse(parameter, &format) < 0 || format.format != SPA_VIDEO_FORMAT_BGRA ||
            !format.size.width || !format.size.height || format.size.width > 4096 || format.size.height > 4096) {
            stream.failed = true; return;
        }
        const auto bytes = static_cast<std::size_t>(format.size.width) * format.size.height * 4 * 2;
        auto& owner = stream.owner;
        if (bytes > stream_limit - (owner.stream_bytes - stream.reserved)) { stream.failed = true; return; }
        owner.stream_bytes = owner.stream_bytes - stream.reserved + bytes;
        stream.reserved = bytes;
        stream.format_width = format.size.width; stream.format_height = format.size.height;
        std::uint8_t storage[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_pod* params[] = {
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<int>(bytes / 2)),
                SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int>(format.size.width * 4)),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd))),
            static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
        };
        if (pw_stream_update_params(stream.stream, params, 2) < 0) stream.failed = true;
    };
    events.process = [](void* data) {
        auto& stream = *static_cast<PortalStream*>(data);
        auto* buffer = pw_stream_dequeue_buffer(stream.stream);
        if (!buffer) return;
        auto* native = buffer->buffer;
        if (!native->n_datas || !native->datas[0].chunk) {
            stream.failed = true; pw_stream_queue_buffer(stream.stream, buffer); return;
        }
        auto& plane = native->datas[0];
        plane.chunk->size = 0;
        plane.chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
        const auto& image = stream.image;
        const auto size = static_cast<std::size_t>(stream.format_width) * stream.format_height * 4;
        if (image && image->width == static_cast<int>(stream.format_width) &&
            image->height == static_cast<int>(stream.format_height) && plane.data && plane.maxsize >= size) {
            const auto* source = cairo_image_surface_get_data(image->surface);
            const auto stride = cairo_image_surface_get_stride(image->surface);
            for (std::uint32_t row = 0; row < stream.format_height; ++row)
                std::memcpy(static_cast<char*>(plane.data) + row * stream.format_width * 4,
                    source + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), stream.format_width * 4);
            plane.chunk->offset = 0; plane.chunk->size = static_cast<std::uint32_t>(size);
            plane.chunk->stride = static_cast<std::int32_t>(stream.format_width * 4); plane.chunk->flags = 0;
            if (auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(native, SPA_META_Header, sizeof(spa_meta_header)))) {
                header->flags = 0; header->seq = stream.sequence++;
                header->pts = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                header->dts_offset = 0;
            }
        }
        pw_stream_queue_buffer(stream.stream, buffer);
    };
    return events;
}();

int open_portal(lua_State* lua) {
    int fps = 30;
    if (!lua_isnoneornil(lua, 1)) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        fps = static_cast<int>(integer_field(lua, 1, "max_fps", 30, 1, 60));
    }
    auto& gui = gui_state(lua);
    if (gui.portal) throw std::runtime_error("portal is already running");
    gui.portal = std::make_unique<Portal>(gui, fps);
    lua_pushboolean(lua, true); return 1;
}

}

void Portal::connect_pipewire() {
    if (loop) return;
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    auto* new_loop = pw_loop_new(nullptr);
    if (!new_loop) throw std::runtime_error("cannot create PipeWire loop");
    pw_loop_enter(new_loop);
    pw_context* new_context = nullptr;
    pw_core* new_core = nullptr;
    try {
        new_context = pw_context_new(new_loop, nullptr, 0);
        if (!new_context) throw std::runtime_error("cannot create PipeWire context");
        new_core = pw_context_connect(new_context, nullptr, 0);
        if (!new_core) throw std::runtime_error("cannot connect to PipeWire");
    } catch (...) {
        if (new_context) pw_context_destroy(new_context);
        pw_loop_leave(new_loop); pw_loop_destroy(new_loop);
        throw;
    }
    loop = new_loop; context = new_context; core = new_core;
    pw_core_add_listener(core, &core_listener, &core_events, this);
}

PortalStream::PortalStream(Portal& owner, const PortalSource& source)
    : owner(owner), type(source.type), x(source.x), y(source.y), width(source.width), height(source.height) {
    stream = pw_stream_new(owner.core, "Hyprworks ScreenCast",
        pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_NODE_NAME, "hyprworks.screencast", PW_KEY_NODE_DESCRIPTION, source.title.c_str(), nullptr));
    if (!stream) throw std::runtime_error("cannot create PipeWire stream");
    pw_stream_add_listener(stream, &listener, &stream_events, this);
}

void PortalStream::publish(std::shared_ptr<Image> frame) {
    if (!image || image->width != frame->width || image->height != frame->height) {
        std::uint8_t storage[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_rectangle size{static_cast<std::uint32_t>(frame->width), static_cast<std::uint32_t>(frame->height)};
        const spa_fraction rate{static_cast<std::uint32_t>(owner.fps), 1};
        const spa_pod* params[] = {static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)))};
        int result;
        if (!image)
            result = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
        else result = pw_stream_update_params(stream, params, 1);
        if (result < 0) throw std::runtime_error("PipeWire format negotiation failed");
    }
    image = std::move(frame);
}

PortalStream::~PortalStream() {
    if (capture) close_capture(*capture);
    spa_hook_remove(&listener); pw_stream_destroy(stream);
    owner.stream_bytes -= reserved;
}

Portal::Portal(Gui& gui, int fps) : gui(gui), fps(fps) {
    sessions.reserve(8);
    ssize_t count;
    do { count = getrandom(&next_token, sizeof(next_token), 0); } while (count < 0 && errno == EINTR);
    if (count != sizeof(next_token)) throw std::runtime_error("cannot allocate portal identity namespace");
    next_token >>= 1;
    require(bus.start(false));
    require(bus.export_interface(path, screencast,
        {{"CreateSession", "oosa{sv}", "ua{sv}"}, {"SelectSources", "oosa{sv}", "ua{sv}"},
         {"Start", "oossa{sv}", "ua{sv}"}}, {},
        {{"version", number(3), true}, {"AvailableSourceTypes", number(0), false},
         {"AvailableCursorModes", number(3), true}}, request_timeout));
    require(bus.export_interface(path, control,
        {{"Attach", "", "taa{sv}aa{sv}"}, {"Snapshot", "", "taa{sv}aa{sv}"},
         {"Choose", "ttas", ""}, {"Cancel", "t", ""}, {"Stop", "t", ""}},
        {{"Changed", ""}, {"Failed", "ts"}}, {}, request_timeout));
    for (const auto* name : {frontend_name, ui_name}) {
        require(bus.subscribe(std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='") + name + "'"));
        const auto id = require(bus.call({"org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetNameOwner", {text(name)}, std::chrono::seconds(5)}));
        (name == frontend_name ? frontend_query : ui_query) = id;
    }
    start_windows(gui);
}

Portal::~Portal() {
    sessions.clear();
    bus.stop();
    if (core) { spa_hook_remove(&core_listener); pw_core_disconnect(core); }
    if (context) pw_context_destroy(context);
    if (loop) { pw_loop_leave(loop); pw_loop_destroy(loop); }
}

void Portal::close_session(std::size_t index, std::uint32_t code, std::string_view error) {
    auto& session = *sessions[index];
    if (!error.empty() && attached)
        (void)bus.emit(path, control, "Failed", {number(session.token, "t"), text(std::string(error))}, ui_owner);
    if (session.request) (void)bus.reply(session.request, response(code));
    if (!session.request_path.empty()) (void)bus.unexport(session.request_path, request_interface);
    (void)bus.emit(session.path, session_interface, "Closed", {}, frontend);
    (void)bus.unexport(session.path, session_interface);
    sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(index));
    changed = true;
}

void Portal::refresh_sources() {
    std::vector<PortalSource> next;
    std::uint32_t available = 0;
    if (gui.capture_manager && gui.output_capture_manager) {
        available |= 1;
        for (const auto& output : gui.outputs) if (output->ready)
            next.push_back({"output:" + std::to_string(output->id), output->state.description.empty() ? output->state.name : output->state.description,
                {}, 1, output->id, output->state.x, output->state.y, output->state.width, output->state.height});
    }
    if (gui.capture_manager && gui.windows && gui.windows->capture_sources) {
        available |= 2;
        if (gui.windows->records) for (const auto& window : *gui.windows->records) {
            if (!window.mapped || window.app_class == "hyprworks-portal") continue;
            if (std::ranges::none_of(gui.windows->targets, [&](const auto& target) { return target->id == window.capture_id; })) continue;
            next.push_back({"window:" + window.id, window.app_class + " — " + window.title, window.id, 2, 0, 0, 0, window.width, window.height});
        }
    }
    if (available != types) {
        require(bus.set_property(path, screencast, "AvailableSourceTypes", number(available)));
        types = available;
    }
    if (next == sources) return;
    if (revision == UINT64_MAX) throw std::runtime_error("portal source revisions exhausted");
    ++revision; sources = std::move(next); changed = true;
}

Body Portal::snapshot() const {
    Body requests, offered;
    for (const auto& session : sessions) {
        static const char* phases[] = {"created", "selected", "choosing", "starting", "streaming"};
        requests.push_back(dictionary({{"id", number(session->token, "t")}, {"app_id", text(session->app)},
            {"parent_window", text(session->parent)}, {"phase", text(phases[static_cast<unsigned>(session->phase)])},
            {"types", number(session->types)}, {"multiple", {"b", session->multiple}}}));
    }
    for (const auto& source : sources)
        offered.push_back(dictionary({{"id", text(source.id)}, {"title", text(source.title)}, {"type", number(source.type)}}));
    return {number(revision, "t"), {"aa{sv}", std::move(requests)}, {"aa{sv}", std::move(offered)}};
}

void Portal::method(const dbus::Event& event) {
    const auto& args = event.arguments;
    if (event.interface == control) {
        if (event.sender != ui_owner) throw std::runtime_error("caller does not own the portal UI name");
        if (event.member == "Attach") { require(bus.reply(event.id, snapshot())); attached = true; return; }
        if (!attached) throw std::runtime_error("UI must attach first");
        if (event.member == "Snapshot") { require(bus.reply(event.id, snapshot())); return; }
        const auto token = integer(args[0]);
        const auto found = std::ranges::find_if(sessions, [&](const auto& session) { return session->token == token; });
        if (found == sessions.end()) throw std::runtime_error("portal request is no longer active");
        auto& session = **found;
        if (event.member == "Cancel" || event.member == "Stop") {
            if (event.member == "Cancel" && session.phase == PortalSession::Phase::streaming)
                throw std::runtime_error("use Stop for a streaming session");
            require(bus.reply(event.id, {}));
            close_session(static_cast<std::size_t>(found - sessions.begin()), 1);
            return;
        }
        if (session.phase != PortalSession::Phase::choosing || integer(args[1]) != revision)
            throw std::runtime_error("selection is stale; refresh the source list");
        const auto& selected = items(args[2]);
        if (selected.empty() || selected.size() > (session.multiple ? 4U : 1U))
            throw std::runtime_error("select one source, or up to four when multiple sources are allowed");
        std::vector<const PortalSource*> chosen;
        for (const auto& choice : selected) {
            const auto source = std::ranges::find(sources, string(choice), &PortalSource::id);
            if (source == sources.end() || !(session.types & source->type)) throw std::runtime_error("source is unavailable");
            if (!std::ranges::contains(chosen, &*source)) chosen.push_back(&*source);
        }
        connect_pipewire();
        std::vector<std::unique_ptr<PortalStream>> streams;
        for (const auto* source : chosen) {
            auto stream = std::make_unique<PortalStream>(*this, *source);
            stream->capture = capture_source(gui, source->window, source->output, session.cursor == 2, fps);
            streams.push_back(std::move(stream));
        }
        require(bus.reply(event.id, {}));
        session.streams = std::move(streams); session.phase = PortalSession::Phase::starting;
        session.deadline = Clock::now() + std::chrono::seconds(10); changed = true;
        return;
    }
    if (event.sender != frontend) throw std::runtime_error("caller does not own the desktop portal name");
    if (event.interface == request_interface || event.interface == session_interface) {
        const bool request = event.interface == request_interface;
        const auto found = std::ranges::find_if(sessions, [&](const auto& session) {
            return (request ? session->request_path : session->path) == event.path;
        });
        if (found == sessions.end()) throw std::runtime_error("session is closed");
        require(bus.reply(event.id, {}));
        close_session(static_cast<std::size_t>(found - sessions.begin()), 1);
        return;
    }
    assert(event.interface == screencast);
    const auto& request_path = string(args[0]);
    const auto& session_path = string(args[1]);
    const auto& app = string(args[2]);
    if (!request_path.starts_with("/org/freedesktop/portal/desktop/request/") ||
        !session_path.starts_with("/org/freedesktop/portal/desktop/session/") ||
        request_path.size() > 1024 || session_path.size() > 1024 || app.size() > 255)
        throw std::runtime_error("invalid portal request identity");
    const auto found = std::ranges::find_if(sessions, [&](const auto& session) { return session->path == session_path; });
    if (event.member == "CreateSession") {
        if (found != sessions.end() || sessions.size() >= 8 || next_token == UINT64_MAX)
            throw std::runtime_error("session already exists or portal capacity reached");
        auto session = std::make_unique<PortalSession>();
        session->path = session_path; session->app = app; session->token = ++next_token;
        session->deadline = Clock::now() + request_timeout;
        require(bus.export_interface(session_path, session_interface, {{"Close", "", ""}}, {{"Closed", ""}},
            {{"version", number(1), true}}, request_timeout));
        sessions.push_back(std::move(session));
        const auto replied = bus.reply(event.id, response(0));
        if (!replied) { close_session(sessions.size() - 1); require(replied); }
        changed = true; return;
    }
    if (found == sessions.end() || (*found)->app != app) throw std::runtime_error("unknown portal session");
    auto& session = **found;
    if (event.member == "SelectSources") {
        if (session.phase != PortalSession::Phase::created) throw std::runtime_error("sources already selected");
        std::uint32_t source_types = 1, cursor_mode = 1;
        bool multiple = false;
        for (const auto& entry : items(args[3])) {
            const auto& pair = items(entry);
            const auto& name = string(pair[0]);
            const auto& value = items(pair[1]).front();
            if (name == "types" || name == "cursor_mode") {
                if (value.signature != "u") throw std::runtime_error("portal source and cursor modes require unsigned integers");
                (name == "types" ? source_types : cursor_mode) = static_cast<std::uint32_t>(integer(value));
            } else if (name == "multiple") {
                if (value.signature != "b") throw std::runtime_error("portal multiple selection requires a boolean");
                multiple = std::get<bool>(value.data);
            }
        }
        if (!source_types || (source_types & ~types) || (cursor_mode != 1 && cursor_mode != 2)) {
            require(bus.reply(event.id, response(2)));
            close_session(static_cast<std::size_t>(found - sessions.begin())); return;
        }
        session.types = source_types; session.cursor = cursor_mode; session.multiple = multiple;
        session.phase = PortalSession::Phase::selected;
        require(bus.reply(event.id, response(0))); changed = true; return;
    }
    if (session.phase != PortalSession::Phase::selected) throw std::runtime_error("session is not ready to start");
    if (string(args[3]).size() > 1024) throw std::runtime_error("parent window identifier exceeds limit");
    std::string parent = string(args[3]), request_object = request_path;
    require(bus.export_interface(request_path, request_interface, {{"Close", "", ""}}, {}, {}, request_timeout));
    session.parent = std::move(parent); session.request = event.id; session.request_path = std::move(request_object);
    session.phase = PortalSession::Phase::choosing; session.deadline = Clock::now() + request_timeout;
    changed = true;
    (void)bus.call({"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "StartServiceByName", {text(ui_name), number(0)}, std::chrono::seconds(5)});
}

void prepare_portal(Portal& portal, WaitSet& wait) {
    portal.bus_wait = portal.bus.prepare_wait();
    portal.bus_offset = wait.append(portal.bus_wait.fd);
    wait.due(portal.bus_wait.deadline);
    if (portal.loop) wait.append({pw_loop_get_fd(portal.loop), POLLIN, 0});
    for (const auto& session : portal.sessions) {
        wait.due(session->deadline);
        for (const auto& stream : session->streams) wait.due(stream->next_process);
    }
    if (portal.changed) wait.due(Clock::time_point::min());
}

void read_portal(Portal& portal, const WaitSet& wait) {
    portal.bus_wait.fd.revents = wait.fds[portal.bus_offset].revents;
    const auto update = portal.bus.dispatch(portal.bus_wait);
    if (portal.bus.status().phase == dbus::Phase::failed) throw std::runtime_error(portal.bus.status().error);
    if (portal.loop && pw_loop_iterate(portal.loop, 0) < 0) portal.pipewire_failed = true;
    portal.refresh_sources();
    for (const auto& event : update.events) {
        if (event.kind == dbus::EventKind::name_failed || event.kind == dbus::EventKind::name_lost)
            throw std::runtime_error("portal backend name is unavailable");
        if (event.kind == dbus::EventKind::reply && (event.id == portal.frontend_query || event.id == portal.ui_query)) {
            auto& owner = event.id == portal.frontend_query ? portal.frontend : portal.ui_owner;
            if (event.error.empty() && event.arguments.size() == 1) owner = string(event.arguments[0]);
        }
        if (event.kind == dbus::EventKind::signal && event.member == "NameOwnerChanged" && event.arguments.size() == 3) {
            const auto& name = string(event.arguments[0]);
            if (name == frontend_name) {
                while (!portal.sessions.empty()) portal.close_session(0);
                portal.frontend = string(event.arguments[2]);
            } else if (name == ui_name) {
                portal.ui_owner = string(event.arguments[2]); portal.attached = false;
            }
        }
        if (event.kind == dbus::EventKind::expired)
            for (std::size_t i = portal.sessions.size(); i-- > 0;)
                if (portal.sessions[i]->request == event.id) portal.close_session(i);
        if (event.kind == dbus::EventKind::method) {
            try { portal.method(event); }
            catch (const std::exception& error) { (void)portal.bus.error(event.id, "org.freedesktop.DBus.Error.Failed", error.what()); }
        }
    }
}

void dispatch_portal(Portal& portal) {
    if (portal.pipewire_failed) throw std::runtime_error("PipeWire connection failed");
    if (!portal.name_requested && portal.gui.windows &&
        (portal.gui.windows->phase == WindowCatalog::Phase::ready || portal.gui.windows->phase == WindowCatalog::Phase::failed)) {
        require(portal.bus.request_name(backend));
        portal.name_requested = true;
    }
    for (std::size_t i = portal.sessions.size(); i-- > 0;) {
        auto& session = *portal.sessions[i];
        if (session.deadline && Clock::now() >= *session.deadline) {
            portal.close_session(i, 2, "Screen-sharing request timed out"); continue;
        }
        std::string failure;
        bool ready = !session.streams.empty();
        for (auto& stream : session.streams) {
            try {
                if (auto image = advance_capture(*stream->capture)) stream->publish(std::move(image));
            } catch (const std::exception& error) { failure = error.what(); }
            if (stream->capture->error) failure = stream->capture->error;
            else if (stream->capture->closed) failure = "capture closed";
            else if (stream->failed) failure = "PipeWire stream failed";
            if (stream->next_process && Clock::now() >= *stream->next_process) {
                if (pw_stream_trigger_process(stream->stream) < 0) failure = "PipeWire frame submission failed";
                stream->next_process = Clock::now() + std::chrono::nanoseconds(1000000000 / portal.fps);
            }
            ready &= stream->state >= PW_STREAM_STATE_PAUSED && pw_stream_get_node_id(stream->stream) != PW_ID_ANY;
        }
        if (!failure.empty()) {
            std::fprintf(stderr, "ScreenCast: %s\n", failure.c_str());
            portal.close_session(i, 2, failure); continue;
        }
        if (ready && session.phase == PortalSession::Phase::starting) {
            Body streams;
            for (const auto& stream : session.streams) {
                auto properties = dictionary({{"source_type", number(stream->type)},
                    {"size", {"(ii)", Body{{"i", std::int64_t(stream->width)}, {"i", std::int64_t(stream->height)}}}}});
                if (stream->type == 1)
                    std::get<Body>(properties.data).push_back({"{sv}", Body{text("position"), {"v", Body{
                        {"(ii)", Body{{"i", std::int64_t(stream->x)}, {"i", std::int64_t(stream->y)}}}}}}});
                streams.push_back({"(ua{sv})", Body{number(pw_stream_get_node_id(stream->stream)), std::move(properties)}});
            }
            if (!portal.bus.reply(session.request, {number(0), dictionary({{"streams", {"a(ua{sv})", std::move(streams)}}})})) {
                portal.close_session(i); continue;
            }
            session.request = 0;
            require(portal.bus.unexport(session.request_path, request_interface)); session.request_path.clear();
            session.phase = PortalSession::Phase::streaming; session.deadline.reset(); portal.changed = true;
        }
    }
    if (std::exchange(portal.changed, false) && portal.attached)
        require(portal.bus.emit(path, control, "Changed", {}, portal.ui_owner));
}

void register_portal(lua_State* lua) {
    lua_pushcfunction(lua, guarded<open_portal>); lua_setfield(lua, -2, "portal");
}

}
