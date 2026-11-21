#pragma once

#include <chrono>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <lua.hpp>
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client.h"
#include "ext-foreign-toplevel-list-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"

namespace hyprworks::lua {

struct Gui;
struct WaitSet;
struct WindowCatalog;
struct Image;
inline constexpr char captured_image_type[] = "hyprworks.captured_image";
struct CaptureTarget {
    WindowCatalog* catalog = nullptr;
    ext_foreign_toplevel_handle_v1* native = nullptr;
    std::string id;
};
struct WindowCapture {
    WindowCatalog* catalog = nullptr;
    std::string capture_id;
    int callback = LUA_NOREF, handle = LUA_NOREF;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    std::size_t bytes = 0;
    std::uint32_t width = 0, height = 0, format = UINT32_MAX, transform = 0;
    std::uint32_t buffer_width = 0, buffer_height = 0, buffer_format = UINT32_MAX;
    int max_width = 448, max_height = 252;
    std::chrono::milliseconds interval{1000};
    std::optional<std::chrono::steady_clock::time_point> next_frame, deadline;
    std::shared_ptr<Image> image;
    const char* error = nullptr;
    unsigned constraint_failures = 0;
    bool closed = false, unavailable = false, constraints = false, collecting = false, ready = false, pending = false;
};
struct WindowRecord {
    std::string id, address, capture_id, app_class, title;
    int width = 0, height = 0;
    bool mapped = false, focused = false;
    bool operator==(const WindowRecord&) const = default;
};

/// Borrows Gui's display. An IPC candidate becomes publishable after a Wayland
/// sync confirms its invalidation generation; restart replaces target identities.
struct WindowCatalog {
    Gui* gui = nullptr;
    wl_registry* registry = nullptr;
    wl_callback* sync = nullptr;
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    ext_foreign_toplevel_list_v1* capture_list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* capture_sources = nullptr;
    ext_image_copy_capture_manager_v1* capture_manager = nullptr;
    std::uint32_t manager_name = 0, capture_list_name = 0, capture_sources_name = 0, capture_manager_name = 0;
    std::vector<zwlr_foreign_toplevel_handle_v1*> signals;
    std::vector<std::unique_ptr<CaptureTarget>> targets;
    std::array<std::shared_ptr<WindowCapture>, 8> captures;
    std::size_t capture_bytes = 0;
    std::shared_ptr<const std::vector<WindowRecord>> records;
    std::string socket_path, identity_prefix, reply, error;
    const char* protocol_error = nullptr;
    enum class Phase { stopped, connecting, synchronizing, ready, failed };
    Phase phase = Phase::stopped;
    enum class Query { idle, connecting, sending, reading, synchronizing, complete };
    Query query = Query::idle;
    int fd = -1, handle = LUA_NOREF, on_change = LUA_NOREF, on_status = LUA_NOREF;
    std::uint64_t generation = 0, revision = 0, invalidation = 0, queried_invalidation = 0;
    bool closed = false, registered = false, subscribed = false;
    bool change_pending = false, status_pending = false;
    std::optional<std::chrono::steady_clock::time_point> refresh_at, deadline;
    std::size_t wait_offset = 0;
};

void register_windows(lua_State* lua);
void close_windows(WindowCatalog& catalog) noexcept;
void prepare_windows(WindowCatalog& catalog, WaitSet& wait);
void read_windows(WindowCatalog& catalog, const WaitSet& wait);
int dispatch_windows(WindowCatalog& catalog);

}
