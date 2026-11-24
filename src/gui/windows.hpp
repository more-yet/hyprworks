#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <lua.hpp>
#include <wayland-client.h>
#include <hyprworks/hyprland/workspaces.hpp>
#include "wlr-foreign-toplevel-management-unstable-v1-client.h"
#include "ext-foreign-toplevel-list-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "ext-workspace-v1-client.h"
#include "hyprland-workspace-image-capture-source-v1-client.h"

namespace hyprworks::lua {

struct Gui;
struct WaitSet;
struct WindowCatalog;
struct WorkspaceCaptureGroup;
inline constexpr char captured_image_type[] = "hyprworks.captured_image";
struct CaptureTarget {
    WindowCatalog* catalog = nullptr;
    ext_foreign_toplevel_handle_v1* native = nullptr;
    std::string id;
};
struct Capture {
    Gui* gui;
    WindowCatalog* catalog = nullptr;
    std::uint32_t output = 0;
    ext_workspace_handle_v1* workspace = nullptr;
    std::string capture_id;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    std::size_t bytes = 0;
    std::uint32_t width = 0, height = 0, format = UINT32_MAX, transform = 0;
    std::uint32_t buffer_width = 0, buffer_height = 0, buffer_format = UINT32_MAX;
    int max_width = 0, max_height = 0;
    std::chrono::milliseconds interval{};
    std::optional<std::chrono::steady_clock::time_point> next_frame, deadline;
    const char* error = nullptr;
    unsigned constraint_failures = 0;
    bool closed = false, unavailable = false, constraints = false, ready = false;
    explicit Capture(Gui& gui) : gui(&gui) {}
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture();
};
struct CaptureBinding {
    Capture capture;
    int callback = LUA_NOREF, handle = LUA_NOREF;
    explicit CaptureBinding(Gui& gui) : capture(gui) {}
};
struct WorkspaceCaptureTarget {
    Gui* gui = nullptr;
    ext_workspace_handle_v1* native = nullptr;
    WorkspaceCaptureGroup* group = nullptr;
    std::string name;
    bool hidden = false;
};
struct WorkspaceCaptureGroup {
    Gui* gui = nullptr;
    ext_workspace_group_handle_v1* native = nullptr;
    std::vector<wl_output*> outputs;
};
struct WindowRecord {
    std::string id, address, capture_id, app_class, title;
    int width = 0, height = 0;
    bool mapped = false, focused = false;
    bool operator==(const WindowRecord&) const = default;
};

struct WindowCatalog {
    Gui* gui = nullptr;
    wl_registry* registry = nullptr;
    wl_callback* sync = nullptr;
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    ext_foreign_toplevel_list_v1* capture_list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* capture_sources = nullptr;
    std::uint32_t manager_name = 0, capture_list_name = 0, capture_sources_name = 0;
    std::vector<zwlr_foreign_toplevel_handle_v1*> signals;
    std::vector<std::unique_ptr<CaptureTarget>> targets;
    std::shared_ptr<const std::vector<WindowRecord>> records;
    std::string socket_path, identity_prefix, reply, error;
    const char* protocol_error = nullptr;
    enum class Phase { stopped, connecting, synchronizing, ready, failed };
    Phase phase = Phase::stopped;
    using Query = hyprworks::hyprland::SocketPhase;
    Query query = Query::idle;
    int fd = -1, handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF;
    std::uint64_t generation = 0, revision = 0, invalidation = 0, queried_invalidation = 0;
    bool closed = false, registered = false;
    bool change_pending = false, status_pending = false;
    std::optional<std::chrono::steady_clock::time_point> refresh_at, deadline;
    std::size_t wait_offset = 0;
};

void register_windows(lua_State* lua);
void start_windows(Gui& gui);
void start_capture(Capture& capture, const std::string& window, std::uint32_t output, bool cursor, int fps);
std::shared_ptr<struct Image> advance_capture(Capture& capture);
void close_capture(Capture& capture);
void close_windows(WindowCatalog& catalog) noexcept;
void prepare_windows(WindowCatalog& catalog, WaitSet& wait);
void read_windows(WindowCatalog& catalog, const WaitSet& wait);
void bind_captures(Gui& gui, wl_registry* registry, std::uint32_t name, const char* interface);
void remove_capture_global(Gui& gui, std::uint32_t name);
void close_captures(Gui& gui) noexcept;
void prepare_captures(Gui& gui, WaitSet& wait);
int dispatch_captures(Gui& gui);

}
