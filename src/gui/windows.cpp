#include "../runtime/invocation.hpp"
#include "gui.hpp"
#include "windows.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <json-c/json.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hyprworks::lua {
namespace {

constexpr char window_type[] = "hyprworks.hyprland.windows";
constexpr char capture_type[] = "hyprworks.capture";
constexpr std::size_t max_reply = 4 * 1024 * 1024, max_windows = 4096;
constexpr std::size_t capture_limit = 128 * 1024 * 1024;
constexpr std::string_view request = "[[BATCH]]j/clients;j/activewindow";
using Clock = std::chrono::steady_clock;

void invalidate(WindowCatalog& catalog) {
    ++catalog.invalidation;
    if (!catalog.refresh_at) catalog.refresh_at = Clock::now() + std::chrono::milliseconds(16);
}

void close_query(WindowCatalog& catalog) {
    if (catalog.fd >= 0) close(std::exchange(catalog.fd, -1));
    catalog.query = WindowCatalog::Query::idle;
    catalog.reply.clear();
}

void capture_error(Capture& capture, const char* message, bool unavailable = false);

void fail(WindowCatalog& catalog, std::string message) {
    for (auto& capture : catalog.gui->captures) if (capture && capture->catalog == &catalog && !capture->closed) {
        capture_error(*capture, "window catalog disconnected", true);
        capture->catalog = nullptr;
    }
    close_windows(catalog);
    catalog.phase = WindowCatalog::Phase::failed;
    catalog.closed = false;
    catalog.error = std::move(message);
    catalog.change_pending = catalog.status_pending = true;
}

void release_buffer(Capture& capture) {
    if (capture.buffer) wl_buffer_destroy(std::exchange(capture.buffer, nullptr));
    if (capture.pixels) munmap(std::exchange(capture.pixels, nullptr), capture.bytes);
    capture.gui->capture_bytes -= capture.bytes;
    capture.bytes = 0;
}

void stop_capture(Capture& capture) {
    if (capture.frame) ext_image_copy_capture_frame_v1_destroy(std::exchange(capture.frame, nullptr));
    if (capture.session) ext_image_copy_capture_session_v1_destroy(std::exchange(capture.session, nullptr));
    release_buffer(capture);
    capture.image.reset(); capture.next_frame.reset(); capture.deadline.reset();
    capture.ready = capture.pending = false;
}

void close_capture(Capture& capture) {
    if (capture.closed) return;
    stop_capture(capture); capture.closed = true;
    auto* lua = capture.gui->owner.lua;
    unref(lua, capture.callback); unref(lua, capture.handle);
}

void capture_error(Capture& capture, const char* message, bool unavailable) {
    if (capture.closed || capture.error) return;
    stop_capture(capture);
    capture.workspace = nullptr;
    capture.error = message; capture.unavailable = unavailable; capture.pending = true;
}

const ext_foreign_toplevel_handle_v1_listener target_listener{
    .closed = [](void* data, auto* native) {
        auto& target = *static_cast<CaptureTarget*>(data);
        auto& catalog = *target.catalog;
        for (auto& capture : catalog.gui->captures)
            if (capture && capture->catalog == &catalog && capture->capture_id == target.id)
                capture_error(*capture, "window is no longer mapped", true);
        ext_foreign_toplevel_handle_v1_destroy(native);
        std::erase_if(catalog.targets, [data](const auto& entry) { return entry.get() == data; });
        invalidate(catalog);
    },
    .done = [](void* data, auto*) { invalidate(*static_cast<CaptureTarget*>(data)->catalog); },
    .title = [](void*, auto*, const char*) {},
    .app_id = [](void*, auto*, const char*) {},
    .identifier = [](void* data, auto*, const char* id) {
        auto& target = *static_cast<CaptureTarget*>(data);
        try {
            if (std::strlen(id) > 256) throw std::runtime_error("capture identity exceeds limit");
            target.id = id;
        } catch (...) { target.catalog->protocol_error = "cannot retain capture identity"; }
    },
};

const ext_foreign_toplevel_list_v1_listener target_list_listener{
    .toplevel = [](void* data, auto*, auto* native) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        try {
            if (catalog.targets.size() == max_windows) throw std::runtime_error("capture target limit exceeded");
            auto target = std::make_unique<CaptureTarget>();
            target->catalog = &catalog; target->native = native;
            ext_foreign_toplevel_handle_v1_add_listener(native, &target_listener, target.get());
            catalog.targets.push_back(std::move(target));
        } catch (...) {
            ext_foreign_toplevel_handle_v1_destroy(native);
            catalog.protocol_error = "cannot retain capture target";
        }
    },
    .finished = [](void* data, auto*) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        catalog.protocol_error = "capture target subscription ended";
    },
};

const ext_image_copy_capture_session_v1_listener capture_session_listener{
    .buffer_size = [](void* data, auto*, std::uint32_t width, std::uint32_t height) {
        auto& capture = *static_cast<Capture*>(data);
        if (std::exchange(capture.constraints, false)) capture.format = UINT32_MAX;
        capture.width = width; capture.height = height;
    },
    .shm_format = [](void* data, auto*, std::uint32_t format) {
        auto& capture = *static_cast<Capture*>(data);
        if (std::exchange(capture.constraints, false)) capture.format = UINT32_MAX;
        if (format == WL_SHM_FORMAT_ARGB8888 || (format == WL_SHM_FORMAT_XRGB8888 && capture.format == UINT32_MAX))
            capture.format = format;
    },
    .dmabuf_device = [](void* data, auto*, wl_array*) {
        auto& capture = *static_cast<Capture*>(data);
        if (std::exchange(capture.constraints, false)) capture.format = UINT32_MAX;
    },
    .dmabuf_format = [](void* data, auto*, std::uint32_t, wl_array*) {
        auto& capture = *static_cast<Capture*>(data);
        if (std::exchange(capture.constraints, false)) capture.format = UINT32_MAX;
    },
    .done = [](void* data, auto*) {
        auto& capture = *static_cast<Capture*>(data);
        capture.constraints = true;
        if (!capture.frame && !capture.ready && !capture.next_frame) capture.next_frame = Clock::now();
    },
    .stopped = [](void* data, auto*) { capture_error(*static_cast<Capture*>(data), "capture unavailable", true); },
};

const ext_image_copy_capture_frame_v1_listener capture_frame_listener{
    .transform = [](void* data, auto*, std::uint32_t transform) { static_cast<Capture*>(data)->transform = transform; },
    .damage = [](void*, auto*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {},
    .presentation_time = [](void*, auto*, std::uint32_t, std::uint32_t, std::uint32_t) {},
    .ready = [](void* data, auto* frame) {
        auto& capture = *static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(frame); capture.frame = nullptr;
        capture.ready = true; capture.constraint_failures = 0;
    },
    .failed = [](void* data, auto* frame, std::uint32_t reason) {
        auto& capture = *static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(frame); capture.frame = nullptr;
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS && ++capture.constraint_failures <= 3)
            capture.next_frame = Clock::now() + capture.interval;
        else capture_error(capture, "frame capture failed", reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    },
};

const ext_workspace_handle_v1_listener workspace_listener{
    .id = [](void*, auto*, const char*) {},
    .name = [](void* data, auto*, const char* name) {
        auto& target = *static_cast<WorkspaceCaptureTarget*>(data);
        target.gui->capture_workspaces_ready = false;
        protocol(*target.gui, [&] {
            if (std::strlen(name) > 4096) throw std::runtime_error("workspace name exceeds limit");
            target.name = name;
        });
    },
    .coordinates = [](void*, auto*, wl_array*) {},
    .state = [](void* data, auto*, std::uint32_t state) {
        auto& target = *static_cast<WorkspaceCaptureTarget*>(data);
        target.gui->capture_workspaces_ready = false;
        target.hidden = state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN;
    },
    .capabilities = [](void*, auto*, std::uint32_t) {},
    .removed = [](void* data, auto* native) {
        auto& gui = *static_cast<WorkspaceCaptureTarget*>(data)->gui;
        gui.capture_workspaces_ready = false;
        for (auto& capture : gui.captures)
            if (capture && capture->workspace == native) capture_error(*capture, "workspace removed", true);
        ext_workspace_handle_v1_destroy(native);
        std::erase_if(gui.capture_targets, [data](const auto& target) { return target.get() == data; });
    },
};

const ext_workspace_group_handle_v1_listener workspace_group_listener{
    .capabilities = [](void*, auto*, std::uint32_t) {},
    .output_enter = [](void* data, auto*, wl_output* output) {
        auto& group = *static_cast<WorkspaceCaptureGroup*>(data);
        group.gui->capture_workspaces_ready = false;
        protocol(*group.gui, [&] {
            if (std::ranges::find(group.outputs, output) != group.outputs.end()) return;
            if (group.outputs.size() == 128) throw std::runtime_error("workspace output limit exceeded");
            group.outputs.push_back(output);
        });
    },
    .output_leave = [](void* data, auto*, wl_output* output) {
        auto& group = *static_cast<WorkspaceCaptureGroup*>(data);
        group.gui->capture_workspaces_ready = false;
        std::erase(group.outputs, output);
    },
    .workspace_enter = [](void* data, auto*, ext_workspace_handle_v1* workspace) {
        auto& group = *static_cast<WorkspaceCaptureGroup*>(data);
        group.gui->capture_workspaces_ready = false;
        static_cast<WorkspaceCaptureTarget*>(ext_workspace_handle_v1_get_user_data(workspace))->group = &group;
    },
    .workspace_leave = [](void* data, auto*, ext_workspace_handle_v1* workspace) {
        auto& gui = *static_cast<WorkspaceCaptureGroup*>(data)->gui;
        gui.capture_workspaces_ready = false;
        static_cast<WorkspaceCaptureTarget*>(ext_workspace_handle_v1_get_user_data(workspace))->group = nullptr;
    },
    .removed = [](void* data, auto* native) {
        auto& gui = *static_cast<WorkspaceCaptureGroup*>(data)->gui;
        gui.capture_workspaces_ready = false;
        ext_workspace_group_handle_v1_destroy(native);
        std::erase_if(gui.capture_groups, [data](const auto& group) { return group.get() == data; });
    },
};

const ext_workspace_manager_v1_listener workspace_manager_listener{
    .workspace_group = [](void* data, auto*, ext_workspace_group_handle_v1* native) {
        auto& gui = *static_cast<Gui*>(data);
        gui.capture_workspaces_ready = false;
        try {
            if (gui.capture_groups.size() == 128) throw std::runtime_error("workspace group limit exceeded");
            auto group = std::make_unique<WorkspaceCaptureGroup>();
            group->gui = &gui; group->native = native;
            ext_workspace_group_handle_v1_add_listener(native, &workspace_group_listener, group.get());
            gui.capture_groups.push_back(std::move(group));
        } catch (...) {
            ext_workspace_group_handle_v1_destroy(native);
            gui.error = "cannot retain workspace capture group";
        }
    },
    .workspace = [](void* data, auto*, ext_workspace_handle_v1* native) {
        auto& gui = *static_cast<Gui*>(data);
        gui.capture_workspaces_ready = false;
        try {
            if (gui.capture_targets.size() == 4096) throw std::runtime_error("workspace target limit exceeded");
            auto target = std::make_unique<WorkspaceCaptureTarget>();
            target->gui = &gui; target->native = native;
            ext_workspace_handle_v1_add_listener(native, &workspace_listener, target.get());
            gui.capture_targets.push_back(std::move(target));
        } catch (...) {
            ext_workspace_handle_v1_destroy(native);
            gui.error = "cannot retain workspace capture target";
        }
    },
    .done = [](void* data, auto*) { static_cast<Gui*>(data)->capture_workspaces_ready = true; },
    .finished = [](void* data, auto* native) {
        auto& gui = *static_cast<Gui*>(data);
        ext_workspace_manager_v1_destroy(native); gui.capture_workspaces = nullptr;
        remove_capture_global(gui, gui.capture_workspaces_name);
    },
};

const zwlr_foreign_toplevel_handle_v1_listener window_listener{
    .title = [](void*, auto*, const char*) {},
    .app_id = [](void*, auto*, const char*) {},
    .output_enter = [](void*, auto*, wl_output*) {},
    .output_leave = [](void*, auto*, wl_output*) {},
    .state = [](void*, auto*, wl_array*) {},
    .done = [](void* data, auto*) { invalidate(*static_cast<WindowCatalog*>(data)); },
    .closed = [](void* data, auto* native) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        zwlr_foreign_toplevel_handle_v1_destroy(native);
        std::erase(catalog.signals, native);
        invalidate(catalog);
    },
    .parent = [](void*, auto*, auto*) {},
};

const zwlr_foreign_toplevel_manager_v1_listener manager_listener{
    .toplevel = [](void* data, auto*, auto* native) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        try {
            if (catalog.signals.size() == max_windows) throw std::runtime_error("window limit exceeded");
            catalog.signals.push_back(native);
            zwlr_foreign_toplevel_handle_v1_add_listener(native, &window_listener, &catalog);
        } catch (...) {
            zwlr_foreign_toplevel_handle_v1_destroy(native);
            catalog.protocol_error = "cannot retain window notification";
        }
    },
    .finished = [](void* data, auto*) { static_cast<WindowCatalog*>(data)->protocol_error = "window subscription ended"; },
};

const wl_registry_listener registry_listener{
    .global = [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        if (!std::strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) && !catalog.capture_list) {
            catalog.capture_list_name = name;
            catalog.capture_list = static_cast<ext_foreign_toplevel_list_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
            if (catalog.capture_list) ext_foreign_toplevel_list_v1_add_listener(catalog.capture_list, &target_list_listener, &catalog);
            return;
        }
        if (!std::strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) && !catalog.capture_sources) {
            catalog.capture_sources_name = name;
            catalog.capture_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
                wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
            return;
        }
        if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) || catalog.manager) return;
        catalog.manager_name = name;
        catalog.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min(version, 3u)));
        if (!catalog.manager) catalog.protocol_error = "cannot bind window notifications";
        else zwlr_foreign_toplevel_manager_v1_add_listener(catalog.manager, &manager_listener, &catalog);
    },
    .global_remove = [](void* data, wl_registry*, std::uint32_t name) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        if (name == catalog.manager_name || name == catalog.capture_list_name ||
            name == catalog.capture_sources_name)
            catalog.protocol_error = "window protocol removed";
    },
};

void synchronize(WindowCatalog& catalog);
const wl_callback_listener sync_listener{
    .done = [](void* data, wl_callback* callback, std::uint32_t) {
        auto& catalog = *static_cast<WindowCatalog*>(data);
        wl_callback_destroy(callback); catalog.sync = nullptr;
        if (catalog.subscribed) { catalog.query = WindowCatalog::Query::complete; return; }
        if (!catalog.manager || !catalog.capture_list) { catalog.protocol_error = "compositor lacks foreign-toplevel notifications"; return; }
        if (!catalog.registered) {
            catalog.registered = true;
            synchronize(catalog);
            return;
        }
        catalog.subscribed = true;
        catalog.phase = WindowCatalog::Phase::synchronizing;
        catalog.status_pending = true; catalog.deadline.reset();
        catalog.refresh_at = Clock::now();
    },
};

void synchronize(WindowCatalog& catalog) {
    catalog.sync = wl_display_sync(catalog.gui->display);
    if (!catalog.sync) catalog.protocol_error = "cannot synchronize window subscription";
    else wl_callback_add_listener(catalog.sync, &sync_listener, &catalog);
}

void start(WindowCatalog& catalog) {
    close_windows(catalog);
    catalog.closed = false;
    auto target = native::session_from_environment();
    if (!target) { fail(catalog, target.error().message); return; }
    catalog.socket_path = target->directory + "/.socket.sock";
    catalog.identity_prefix = target->directory + ":" + std::to_string(catalog.generation) + ":";
    catalog.phase = WindowCatalog::Phase::connecting;
    catalog.status_pending = true;
    catalog.deadline = Clock::now() + std::chrono::seconds(2);
    catalog.registry = wl_display_get_registry(catalog.gui->display);
    if (!catalog.registry) { fail(catalog, "cannot create window registry"); return; }
    wl_registry_add_listener(catalog.registry, &registry_listener, &catalog);
    synchronize(catalog);
}

json_object* member(json_object* object, const char* key, json_type type) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, type))
        throw std::runtime_error(std::string("invalid window field: ") + key);
    return value;
}

std::string text(json_object* object, const char* key) {
    auto* value = member(object, key, json_type_string);
    std::string result(json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value)));
    if (result.size() > 16384 || result.contains('\0')) throw std::runtime_error("invalid window text");
    return result;
}

std::unique_ptr<json_object, decltype(&json_object_put)> parse(std::string_view input, json_type type) {
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tokener(json_tokener_new_ex(32), json_tokener_free);
    if (!tokener) throw std::bad_alloc();
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    std::unique_ptr<json_object, decltype(&json_object_put)> value(
        json_tokener_parse_ex(tokener.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    if (json_tokener_get_error(tokener.get()) != json_tokener_success ||
        json_tokener_get_parse_end(tokener.get()) != input.size() || !json_object_is_type(value.get(), type))
        throw std::runtime_error("invalid window snapshot JSON");
    return value;
}

std::vector<WindowRecord> snapshot(WindowCatalog& catalog) {
    constexpr std::string_view separator = "\n\n\n";
    const auto split = catalog.reply.find(separator);
    if (split == std::string::npos) throw std::runtime_error("missing active-window reply");
    auto clients = parse(std::string_view(catalog.reply).substr(0, split), json_type_array);
    auto active = parse(std::string_view(catalog.reply).substr(split + separator.size()), json_type_object);
    json_object* focused = nullptr;
    const std::string address = json_object_object_get_ex(active.get(), "address", &focused) ? text(active.get(), "address") : "";
    const auto count = json_object_array_length(clients.get());
    if (count > max_windows) throw std::runtime_error("window snapshot exceeds 4096 records");
    std::vector<WindowRecord> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto* object = json_object_array_get_idx(clients.get(), i);
        WindowRecord record;
        record.address = text(object, "address");
        record.capture_id = text(object, "stableId");
        if (!record.address.starts_with("0x") || record.address.size() <= 2 || record.address.size() > 18 ||
            record.address.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos ||
            record.capture_id.empty() || record.capture_id.size() > 16 ||
            record.capture_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("invalid window identity");
        record.id = catalog.identity_prefix + record.capture_id;
        record.app_class = text(object, "class"); record.title = text(object, "title");
        record.mapped = json_object_get_boolean(member(object, "mapped", json_type_boolean));
        record.focused = record.mapped && record.address == address;
        auto* size = member(object, "size", json_type_array);
        if (json_object_array_length(size) != 2) throw std::runtime_error("invalid window dimensions");
        int* dimensions[] = {&record.width, &record.height};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            auto* dimension = json_object_array_get_idx(size, axis);
            if (!json_object_is_type(dimension, json_type_int)) throw std::runtime_error("invalid window dimension");
            const auto pixels = json_object_get_int64(dimension);
            if (pixels < 0 || pixels > 1048576) throw std::runtime_error("window dimension out of range");
            *dimensions[axis] = static_cast<int>(pixels);
        }
        records.push_back(std::move(record));
    }
    std::ranges::sort(records, {}, &WindowRecord::id);
    for (std::size_t i = 1; i < records.size(); ++i)
        if (records[i].id == records[i - 1].id) throw std::runtime_error("duplicate window identity");
    return records;
}

WindowCatalog& catalog(lua_State* lua) {
    auto& owner = invocation(lua);
    if (owner.closing || !owner.gui) luaL_error(lua, "window subscription requires a live GUI");
    luaL_checkudata(lua, 1, window_type);
    return *owner.gui->windows;
}

int open(lua_State* lua) {
    auto& owner = invocation(lua);
    if (owner.closing || !owner.gui) return luaL_error(lua, "window subscription requires a live GUI");
    if (!owner.gui->windows) {
        owner.gui->windows = std::make_unique<WindowCatalog>();
        owner.gui->windows->gui = owner.gui.get();
        start(*owner.gui->windows);
    }
    auto& value = *owner.gui->windows;
    return retain_handle(lua, value, window_type);
}

int get_snapshot(lua_State* lua) {
    auto& value = catalog(lua);
    if (!value.records) { lua_pushnil(lua); return 1; }
    const auto records = value.records;
    const auto revision = value.revision;
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 2);
        identifier(lua, revision); lua_setfield(lua, -2, "revision");
        lua_createtable(lua, static_cast<int>(records->size()), 0);
        lua_Integer index = 1;
        for (const auto& record : *records) {
            lua_createtable(lua, 0, 9);
            string_field(lua, "id", record.id); string_field(lua, "address", record.address);
            string_field(lua, "capture_id", record.capture_id);
            string_field(lua, "class", record.app_class); string_field(lua, "title", record.title);
            lua_pushinteger(lua, record.width); lua_setfield(lua, -2, "width");
            lua_pushinteger(lua, record.height); lua_setfield(lua, -2, "height");
            lua_pushboolean(lua, record.mapped); lua_setfield(lua, -2, "mapped");
            lua_pushboolean(lua, record.focused); lua_setfield(lua, -2, "focused");
            lua_rawseti(lua, -2, index++);
        }
        lua_setfield(lua, -2, "windows"); return 1;
    }, 1);
}

int get_status(lua_State* lua) {
    const auto& value = catalog(lua);
    const auto phase = value.phase;
    const auto error = value.error;
    const bool pending = value.refresh_at.has_value() || value.query != WindowCatalog::Query::idle, ready = bool(value.records);
    return protected_call(lua, [&](lua_State* lua) {
        static const char* phases[] = {"closed", "connecting", "synchronizing", "ready", "failed"};
        lua_createtable(lua, 0, 4); string_field(lua, "phase", phases[static_cast<unsigned>(phase)]);
        lua_pushboolean(lua, pending); lua_setfield(lua, -2, "refresh_pending");
        lua_pushboolean(lua, ready); lua_setfield(lua, -2, "has_snapshot");
        if (!error.empty()) string_field(lua, "error", error);
        return 1;
    }, 1);
}

int on_change(lua_State* lua) {
    auto& value = catalog(lua);
    value.change_pending = replace_callback(lua, 2, value.on_change, value.closed);
    return 0;
}
int on_status(lua_State* lua) {
    auto& value = catalog(lua);
    value.status_pending = replace_callback(lua, 2, value.on_status, value.closed);
    return 0;
}
int restart(lua_State* lua) { start(catalog(lua)); lua_pushboolean(lua, true); return 1; }
int shutdown(lua_State* lua) {
    auto& value = catalog(lua); close_windows(value);
    unref(lua, value.on_change); unref(lua, value.on_status);
    return 0;
}

int capture_close(lua_State* lua) {
    auto& capture = *static_cast<std::shared_ptr<Capture>*>(luaL_checkudata(lua, 1, capture_type));
    close_capture(*capture);
    return 0;
}

int capture_gc(lua_State* lua) {
    auto* capture = static_cast<std::shared_ptr<Capture>*>(luaL_checkudata(lua, 1, capture_type));
    if (*capture) close_capture(**capture);
    capture->~shared_ptr();
    return 0;
}

int image_gc(lua_State* lua) {
    static_cast<std::shared_ptr<Image>*>(luaL_checkudata(lua, 1, captured_image_type))->~shared_ptr();
    return 0;
}

template<bool workspace>
int capture_open(lua_State* lua) {
    const int options = workspace ? 1 : 3, callback = workspace ? 2 : 4;
    std::size_t length = 0;
    const char* id = workspace ? nullptr : check_string(lua, 2, &length);
    luaL_checktype(lua, options, LUA_TTABLE); luaL_checktype(lua, callback, LUA_TFUNCTION);
    const auto max_width = static_cast<int>(integer_field(lua, options, "max_width", std::nullopt, 1, 1024));
    const auto max_height = static_cast<int>(integer_field(lua, options, "max_height", std::nullopt, 1, 1024));
    const auto max_fps = integer_field(lua, options, "max_fps", std::nullopt, 1, 30);
    auto& gui = gui_state(lua);
    auto* window_catalog = workspace ? nullptr : &catalog(lua);
    std::string name, output;
    int mode = 0;
    if (workspace) {
        lua_pushvalue(lua, options);
        const int parsed = protected_call(lua, [&](lua_State* lua) {
            text_field(lua, 2, "name", name, 4096);
            text_field(lua, 2, "output", output, 256);
            static const char* modes[] = {"windows_only", "everything", nullptr};
            mode = enum_field(lua, 2, "mode", nullptr, modes);
            if (name.empty() || output.empty()) return luaL_error(lua, "workspace capture requires name and output");
            return 0;
        }, 0, 0, 1);
        if (parsed < 0) return parsed;
    }
    if (!gui.capture_manager) throw std::runtime_error("native image capture is unavailable");
    auto capture = std::make_shared<Capture>();
    capture->gui = &gui;
    std::uint64_t generation = 0;
    if (workspace) {
        if (!gui.workspace_capture_manager || !gui.capture_workspaces)
            throw std::runtime_error("native workspace capture is unavailable");
        if (!gui.capture_workspaces_ready) throw std::runtime_error("workspace capture targets are not synchronized");
        const auto monitor = std::ranges::find_if(gui.outputs, [&](const auto& item) { return item->state.name == output; });
        if (monitor == gui.outputs.end()) throw std::runtime_error("workspace output is unavailable");
        for (const auto& target : gui.capture_targets) {
            if (target->hidden || target->name != name) continue;
            if (!target->group || std::ranges::find(target->group->outputs, (*monitor)->native) == target->group->outputs.end()) continue;
            if (capture->workspace) throw std::runtime_error("workspace capture name is ambiguous on this output");
            capture->workspace = target->native;
        }
        if (!capture->workspace) throw std::runtime_error("workspace capture target is unavailable");
    } else {
        auto& value = *window_catalog;
        if (!value.capture_sources || !value.capture_list) throw std::runtime_error("native window capture is unavailable");
        if (!value.records) throw std::runtime_error("window catalog is not synchronized");
        const auto record = std::ranges::find(*value.records, std::string_view(id, length), &WindowRecord::id);
        if (record == value.records->end() || !record->mapped) throw std::runtime_error("window identity is no longer mapped");
        capture->catalog = &value; capture->capture_id = record->capture_id;
        generation = value.generation;
    }
    auto slot = std::ranges::find_if(gui.captures, [](const auto& item) { return !item || item->closed; });
    if (slot == gui.captures.end()) throw std::runtime_error("at most eight captures may be active");
    capture->max_width = max_width; capture->max_height = max_height;
    capture->interval = std::chrono::milliseconds((1000 + max_fps - 1) / max_fps);
    lua_pushvalue(lua, callback);
    const int result = protected_call(lua, [&](lua_State* lua) {
        auto* handle = new (lua_newuserdatauv(lua, sizeof(std::shared_ptr<Capture>), 0)) std::shared_ptr<Capture>();
        luaL_setmetatable(lua, capture_type);
        *handle = capture;
        lua_pushvalue(lua, -1); capture->handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        lua_pushvalue(lua, 2); capture->callback = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }, 1, 0, 1);
    if (result < 0) { close_capture(*capture); return result; }
    if (capture->closed || gui.owner.closing || (capture->catalog && capture->catalog->generation != generation) ||
        (*slot && !(*slot)->closed)) {
        close_capture(*capture);
        throw std::runtime_error("capture owner changed while opening capture");
    }
    ext_image_capture_source_v1* source = nullptr;
    if (workspace) {
        source = hyprland_workspace_image_capture_source_manager_v1_create_source(
            gui.workspace_capture_manager, capture->workspace, static_cast<std::uint32_t>(mode));
    } else {
        const auto& value = *capture->catalog;
        const auto target = std::ranges::find_if(value.targets, [&](const auto& item) { return item->id == capture->capture_id; });
        if (target == value.targets.end()) {
            close_capture(*capture);
            throw std::runtime_error("window capture target is unavailable");
        }
        source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(value.capture_sources, (*target)->native);
    }
    if (!source) { close_capture(*capture); throw std::runtime_error("cannot create capture source"); }
    capture->session = ext_image_copy_capture_manager_v1_create_session(gui.capture_manager, source, 0);
    ext_image_capture_source_v1_destroy(source);
    if (!capture->session) { close_capture(*capture); throw std::runtime_error("cannot create capture session"); }
    ext_image_copy_capture_session_v1_add_listener(capture->session, &capture_session_listener, capture.get());
    capture->deadline = Clock::now() + std::chrono::seconds(2);
    *slot = std::move(capture);
    return 1;
}

void advance_capture(Capture& capture) {
    if (capture.closed || capture.error || !capture.session) return;
    const auto now = Clock::now();
    if (capture.deadline && now >= *capture.deadline) {
        capture_error(capture, "initial capture frame timed out", true);
        return;
    }
    try {
        if (std::exchange(capture.ready, false)) {
            if (!capture.constraints || capture.width != capture.buffer_width || capture.height != capture.buffer_height ||
                capture.format != capture.buffer_format) {
                capture.next_frame = now;
                return;
            }
            if (capture.transform != WL_OUTPUT_TRANSFORM_NORMAL) {
                capture_error(capture, "unsupported capture transform");
                return;
            }
            auto image = std::make_shared<Image>(capture.gui->images);
            const double scale = std::min({1.0, static_cast<double>(capture.max_width) / capture.buffer_width,
                static_cast<double>(capture.max_height) / capture.buffer_height});
            image->width = std::max(1, static_cast<int>(std::floor(capture.buffer_width * scale)));
            image->height = std::max(1, static_cast<int>(std::floor(capture.buffer_height * scale)));
            image->allocate();
            std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> source(cairo_image_surface_create_for_data(
                static_cast<unsigned char*>(capture.pixels),
                capture.buffer_format == WL_SHM_FORMAT_ARGB8888 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
                static_cast<int>(capture.buffer_width), static_cast<int>(capture.buffer_height),
                static_cast<int>(capture.buffer_width * 4)), cairo_surface_destroy);
            std::unique_ptr<cairo_t, decltype(&cairo_destroy)> paint(cairo_create(image->surface), cairo_destroy);
            cairo_scale(paint.get(), static_cast<double>(image->width) / capture.buffer_width,
                static_cast<double>(image->height) / capture.buffer_height);
            cairo_set_source_surface(paint.get(), source.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(paint.get()), CAIRO_FILTER_GOOD);
            cairo_set_operator(paint.get(), CAIRO_OPERATOR_SOURCE);
            cairo_paint(paint.get());
            if (cairo_status(paint.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("cannot scale captured frame");
            image->submitted = true; image->ready.store(true, std::memory_order_release);
            capture.image = std::move(image); capture.pending = true; capture.deadline.reset();
            capture.next_frame = now + capture.interval;
        }
        if (!capture.constraints || capture.frame || capture.pending || !capture.next_frame || now < *capture.next_frame) return;
        const bool replace = !capture.buffer || capture.buffer_width != capture.width || capture.buffer_height != capture.height ||
            capture.buffer_format != capture.format;
        if (replace) {
            if (!capture.width || !capture.height || capture.width > 8192 || capture.height > 8192 ||
                static_cast<std::uint64_t>(capture.width) * capture.height * 4 > 64 * 1024 * 1024) {
                capture_error(capture, "capture buffer exceeds 64 MiB");
                return;
            }
            if (capture.format == UINT32_MAX) {
                capture_error(capture, "capture requires ARGB8888 or XRGB8888 shared memory", true);
                return;
            }
            release_buffer(capture);
            const auto bytes = static_cast<std::size_t>(capture.width) * capture.height * 4;
            if (bytes > capture_limit - capture.gui->capture_bytes) {
                capture_error(capture, "capture memory limit reached (128 MiB)");
                return;
            }
            const int fd = memfd_create("hyprworks-capture", MFD_CLOEXEC);
            if (fd < 0) throw std::runtime_error("cannot create capture buffer");
            if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) { close(fd); throw std::runtime_error("cannot size capture buffer"); }
            void* pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (pixels == MAP_FAILED) { close(fd); throw std::runtime_error("cannot map capture buffer"); }
            capture.pixels = pixels; capture.bytes = bytes; capture.gui->capture_bytes += bytes;
            auto* pool = wl_shm_create_pool(capture.gui->shm, fd, static_cast<int>(bytes));
            close(fd);
            if (!pool) throw std::runtime_error("cannot create capture pool");
            capture.buffer = wl_shm_pool_create_buffer(pool, 0, static_cast<int>(capture.width), static_cast<int>(capture.height),
                static_cast<int>(capture.width * 4), capture.format);
            wl_shm_pool_destroy(pool);
            if (!capture.buffer) throw std::runtime_error("cannot create capture wl_buffer");
            capture.buffer_width = capture.width; capture.buffer_height = capture.height; capture.buffer_format = capture.format;
        }
        capture.frame = ext_image_copy_capture_session_v1_create_frame(capture.session);
        if (!capture.frame) throw std::runtime_error("cannot create capture frame");
        ext_image_copy_capture_frame_v1_add_listener(capture.frame, &capture_frame_listener, &capture);
        ext_image_copy_capture_frame_v1_attach_buffer(capture.frame, capture.buffer);
        if (replace || capture.constraint_failures)
            ext_image_copy_capture_frame_v1_damage_buffer(capture.frame, 0, 0,
                static_cast<int>(capture.buffer_width), static_cast<int>(capture.buffer_height));
        ext_image_copy_capture_frame_v1_capture(capture.frame);
        capture.next_frame.reset();
    } catch (const std::exception&) { capture_error(capture, "cannot allocate or scale captured frame"); }
}

}

void bind_captures(Gui& gui, wl_registry* registry, std::uint32_t name, const char* interface) {
    if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) && !gui.capture_manager) {
        gui.capture_manager = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
        if (!gui.capture_manager) throw std::runtime_error("cannot bind image capture");
        gui.capture_manager_name = name;
    }
    if (!std::strcmp(interface, hyprland_workspace_image_capture_source_manager_v1_interface.name) && !gui.workspace_capture_manager) {
        gui.workspace_capture_manager = static_cast<hyprland_workspace_image_capture_source_manager_v1*>(
            wl_registry_bind(registry, name, &hyprland_workspace_image_capture_source_manager_v1_interface, 1));
        if (!gui.workspace_capture_manager) throw std::runtime_error("cannot bind workspace capture");
        gui.workspace_capture_manager_name = name;
    }
    if (!std::strcmp(interface, ext_workspace_manager_v1_interface.name) && !gui.capture_workspaces) {
        gui.capture_workspaces = static_cast<ext_workspace_manager_v1*>(wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, 1));
        if (!gui.capture_workspaces) throw std::runtime_error("cannot bind workspace capture targets");
        gui.capture_workspaces_name = name;
        ext_workspace_manager_v1_add_listener(gui.capture_workspaces, &workspace_manager_listener, &gui);
    }
}

void remove_capture_global(Gui& gui, std::uint32_t name) {
    for (const auto& output : gui.outputs) if (output->id == name)
        for (auto& group : gui.capture_groups) std::erase(group->outputs, output->native);
    if (name != gui.capture_manager_name && name != gui.workspace_capture_manager_name && name != gui.capture_workspaces_name) return;
    for (auto& capture : gui.captures)
        if (capture && (name == gui.capture_manager_name || capture->workspace))
            capture_error(*capture, "capture protocol removed", true);
    if (name == gui.capture_manager_name) {
        ext_image_copy_capture_manager_v1_destroy(std::exchange(gui.capture_manager, nullptr));
        gui.capture_manager_name = 0;
    }
    if (name == gui.workspace_capture_manager_name) {
        hyprland_workspace_image_capture_source_manager_v1_destroy(std::exchange(gui.workspace_capture_manager, nullptr));
        gui.workspace_capture_manager_name = 0;
    }
    if (name == gui.capture_workspaces_name) {
        if (gui.capture_workspaces) {
            ext_workspace_manager_v1_stop(gui.capture_workspaces);
            ext_workspace_manager_v1_destroy(std::exchange(gui.capture_workspaces, nullptr));
        }
        for (auto& target : gui.capture_targets) ext_workspace_handle_v1_destroy(target->native);
        for (auto& group : gui.capture_groups) ext_workspace_group_handle_v1_destroy(group->native);
        gui.capture_targets.clear(); gui.capture_groups.clear();
        gui.capture_workspaces_ready = false; gui.capture_workspaces_name = 0;
    }
}

void close_captures(Gui& gui) noexcept {
    for (auto& capture : gui.captures) if (capture) close_capture(*capture);
    gui.captures.fill(nullptr);
    if (gui.capture_manager_name) remove_capture_global(gui, gui.capture_manager_name);
    if (gui.workspace_capture_manager_name) remove_capture_global(gui, gui.workspace_capture_manager_name);
    if (gui.capture_workspaces_name) remove_capture_global(gui, gui.capture_workspaces_name);
}

void close_windows(WindowCatalog& catalog) noexcept {
    for (auto& capture : catalog.gui->captures)
        if (capture && capture->catalog == &catalog) { close_capture(*capture); capture.reset(); }
    for (auto& target : catalog.targets) ext_foreign_toplevel_handle_v1_destroy(target->native);
    catalog.targets.clear();
    if (catalog.capture_list) ext_foreign_toplevel_list_v1_destroy(std::exchange(catalog.capture_list, nullptr));
    if (catalog.capture_sources) ext_foreign_toplevel_image_capture_source_manager_v1_destroy(std::exchange(catalog.capture_sources, nullptr));
    close_query(catalog);
    if (catalog.sync) wl_callback_destroy(std::exchange(catalog.sync, nullptr));
    for (auto* signal : catalog.signals) zwlr_foreign_toplevel_handle_v1_destroy(signal);
    catalog.signals.clear();
    if (catalog.manager) zwlr_foreign_toplevel_manager_v1_destroy(std::exchange(catalog.manager, nullptr));
    if (catalog.registry) wl_registry_destroy(std::exchange(catalog.registry, nullptr));
    catalog.manager_name = catalog.capture_list_name = catalog.capture_sources_name = 0;
    catalog.protocol_error = nullptr; catalog.error.clear();
    catalog.registered = catalog.subscribed = false;
    catalog.change_pending = catalog.status_pending = false;
    catalog.records.reset(); catalog.refresh_at.reset(); catalog.deadline.reset();
    catalog.closed = true; catalog.phase = WindowCatalog::Phase::stopped; ++catalog.generation;
}

void prepare_windows(WindowCatalog& catalog, WaitSet& wait) {
    catalog.wait_offset = wait.fds.size();
    wait.fds.push_back({catalog.fd, static_cast<short>(catalog.query == WindowCatalog::Query::reading ? POLLIN : POLLOUT), 0});
    const auto deadline = [&](Clock::time_point time) { if (!wait.deadline || time < *wait.deadline) wait.deadline = time; };
    if (catalog.protocol_error || catalog.change_pending || catalog.status_pending) deadline(Clock::time_point::min());
    if (catalog.deadline) deadline(*catalog.deadline);
    if (catalog.subscribed && catalog.query == WindowCatalog::Query::idle && catalog.refresh_at) deadline(*catalog.refresh_at);
}

void prepare_captures(Gui& gui, WaitSet& wait) {
    const auto deadline = [&](Clock::time_point time) { if (!wait.deadline || time < *wait.deadline) wait.deadline = time; };
    for (auto& capture : gui.captures) if (capture && !capture->closed) {
        if (capture->ready || capture->pending) deadline(Clock::time_point::min());
        if (capture->constraints && capture->next_frame) deadline(*capture->next_frame);
        if (capture->deadline) deadline(*capture->deadline);
    }
}

void read_windows(WindowCatalog& catalog, const WaitSet& wait) {
    if (catalog.protocol_error) { fail(catalog, catalog.protocol_error); return; }
    const auto now = Clock::now();
    if (catalog.deadline && now >= *catalog.deadline) { fail(catalog, "window subscription timed out"); return; }
    if (!catalog.subscribed) return;
    if (catalog.query == WindowCatalog::Query::synchronizing) return;
    if (catalog.query == WindowCatalog::Query::complete) {
        try {
            if (catalog.queried_invalidation == catalog.invalidation) {
                auto records = snapshot(catalog);
                if (!catalog.records || records != *catalog.records) {
                    catalog.records = std::make_shared<const std::vector<WindowRecord>>(std::move(records));
                    ++catalog.revision; catalog.change_pending = true;
                }
                if (catalog.phase != WindowCatalog::Phase::ready) catalog.status_pending = true;
                catalog.phase = WindowCatalog::Phase::ready;
            } else catalog.refresh_at = now + std::chrono::milliseconds(16);
            close_query(catalog); catalog.deadline.reset();
        } catch (const std::exception& error) { fail(catalog, error.what()); }
        return;
    }
    if (catalog.fd < 0) {
        if (!catalog.refresh_at || now < *catalog.refresh_at) return;
        catalog.refresh_at.reset();
        catalog.queried_invalidation = catalog.invalidation;
        catalog.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (catalog.fd < 0) { fail(catalog, std::strerror(errno)); return; }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, catalog.socket_path.c_str(), catalog.socket_path.size() + 1);
        const int result = connect(catalog.fd, reinterpret_cast<sockaddr*>(&address),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + catalog.socket_path.size() + 1));
        if (result < 0 && errno != EINPROGRESS) { fail(catalog, std::strerror(errno)); return; }
        catalog.query = result == 0 ? WindowCatalog::Query::sending : WindowCatalog::Query::connecting;
        catalog.deadline = now + std::chrono::seconds(2);
        return;
    }
    if (catalog.wait_offset >= wait.fds.size() || wait.fds[catalog.wait_offset].fd != catalog.fd) return;
    const auto events = wait.fds[catalog.wait_offset].revents;
    if (!events) return;
    if (catalog.query == WindowCatalog::Query::connecting) {
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(catalog.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            fail(catalog, std::strerror(error ? error : errno)); return;
        }
        catalog.query = WindowCatalog::Query::sending;
    }
    if (catalog.query == WindowCatalog::Query::sending) {
        const auto count = send(catalog.fd, request.data(), request.size(), MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
        // Socket1 treats its first read as a request; a short write ends this query.
        if (count != static_cast<ssize_t>(request.size())) { fail(catalog, "incomplete window snapshot request"); return; }
        catalog.query = WindowCatalog::Query::reading;
        return;
    }
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        char bytes[4096];
        const auto count = recv(catalog.fd, bytes, sizeof(bytes), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !(events & (POLLERR | POLLHUP))) return;
        if (count < 0) { fail(catalog, std::strerror(errno)); return; }
        if (count > 0) {
            if (catalog.reply.size() + static_cast<std::size_t>(count) > max_reply) { fail(catalog, "window snapshot exceeds 4 MiB"); return; }
            catalog.reply.append(bytes, static_cast<std::size_t>(count));
            continue;
        }
        close(std::exchange(catalog.fd, -1));
        catalog.query = WindowCatalog::Query::synchronizing;
        synchronize(catalog);
        return;
    }
}

int dispatch_windows(WindowCatalog& catalog) {
    auto* lua = catalog.gui->owner.lua;
    const auto generation = catalog.generation;
    const bool status = std::exchange(catalog.status_pending, false);
    const bool change = std::exchange(catalog.change_pending, false);
    if (status && dispatch_callback(lua, catalog, catalog.on_status, generation, [](lua_State*) { return 0; }) < 0) return -1;
    if (change && dispatch_callback(lua, catalog, catalog.on_change, generation, [](lua_State*) { return 0; }) < 0) return -1;
    return 0;
}

int dispatch_captures(Gui& gui) {
    auto* lua = gui.owner.lua;
    for (auto capture : gui.captures) {
        if (!capture || capture->closed || gui.owner.requests.load(std::memory_order_relaxed)) continue;
        advance_capture(*capture);
        if (!std::exchange(capture->pending, false) || capture->callback < 0) continue;
        const auto image = capture->image;
        const int reference = capture->callback;
        lua_pushcfunction(lua, traceback);
        if (protected_call(lua, [&](lua_State* lua) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, capture->handle);
            lua_createtable(lua, 0, 6);
            string_field(lua, "kind", capture->error ? (capture->unavailable ? "unavailable" : "error") : "frame");
            if (capture->error) string_field(lua, "error", capture->error);
            else {
                auto* source = new (lua_newuserdatauv(lua, sizeof(std::shared_ptr<Image>), 0)) std::shared_ptr<Image>();
                luaL_setmetatable(lua, captured_image_type); *source = image;
                lua_setfield(lua, -2, "source");
                lua_pushinteger(lua, image->width); lua_setfield(lua, -2, "width");
                lua_pushinteger(lua, image->height); lua_setfield(lua, -2, "height");
                lua_pushinteger(lua, capture->buffer_width); lua_setfield(lua, -2, "source_width");
                lua_pushinteger(lua, capture->buffer_height); lua_setfield(lua, -2, "source_height");
            }
            if (capture->closed || capture->callback != reference) return 0;
            lua_call(lua, 2, 0); return 0;
        }, 0, 1) < 0) return -1;
        lua_settop(lua, 0);
    }
    return 0;
}

void register_windows(lua_State* lua) {
    luaL_newmetatable(lua, window_type);
    const luaL_Reg methods[] = {{"snapshot", guarded<get_snapshot>}, {"status", guarded<get_status>},
        {"on_change", guarded<on_change>}, {"on_status", guarded<on_status>},
        {"restart", guarded<restart>}, {"close", guarded<shutdown>}, {"capture", operation_call<capture_open<false>>}, {nullptr, nullptr}};
    luaL_setfuncs(lua, methods, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<shutdown>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks window catalog"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    lua_pushcfunction(lua, guarded<open>); lua_setfield(lua, -2, "windows");
    lua_pushcfunction(lua, operation_call<capture_open<true>>); lua_setfield(lua, -2, "capture_workspace");
    luaL_newmetatable(lua, capture_type);
    lua_pushcfunction(lua, guarded<capture_close>); lua_setfield(lua, -2, "close");
    lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<capture_close>); lua_setfield(lua, -2, "__close");
    lua_pushcfunction(lua, guarded<capture_gc>); lua_setfield(lua, -2, "__gc");
    lua_pushliteral(lua, "hyprworks capture"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    luaL_newmetatable(lua, captured_image_type);
    lua_pushcfunction(lua, guarded<image_gc>); lua_setfield(lua, -2, "__gc");
    lua_pushliteral(lua, "hyprworks captured image"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
}

}
