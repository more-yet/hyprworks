#pragma once

#include "images.hpp"
#include <lua.hpp>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <poll.h>
#include <wayland-client.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "xdg-shell-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"
#include "viewporter-client.h"
#include "fractional-scale-v1-client.h"
#include "cursor-shape-v1-client.h"
#include "text-input-unstable-v3-client.h"
#include "hyprland-focus-grab-v1-client.h"

namespace hyprworks::lua {

struct InvocationState;
struct WaitSet;
struct Gui;

struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum class Kind { row, column, overlay, scroll, box, text, image, button, toggle, slider, input };
struct Element {
    Kind kind = Kind::box;
    int parent = -1, end = 0;
    std::string id, text, placeholder, font = "Sans 12";
    double width = -1, height = -1, grow = 0, padding = 0, gap = 0, radius = 0;
    double x = 0, y = 0, border_width = 0;
    int halign = 0, valign = 0, align = 0;
    double value = 0, scroll = 0, natural_w = 0, natural_h = 0;
    std::uint32_t background = 0, color = 0xffffffff, accent = 0x8ab4f8ff, border_color = 0;
    bool enabled = true, wrap = false, password = false, horizontal = false;
    int callback = -2;
    Box box, clip;
    PangoLayout* layout = nullptr;
    std::shared_ptr<Image> image;
    bool image_notified = false;
    std::uint64_t edit_revision = 0;
    int cursor = 0, anchor = 0;
    std::string preedit;
    std::uint32_t preedit_seat = 0;
    int preedit_cursor = 0, preedit_end = 0;
};

struct Surface {
    Gui* gui = nullptr;
    std::uint64_t id = 0, revision = 0, parent = 0;
    wl_surface* native = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    xdg_popup* popup = nullptr;
    zwlr_layer_surface_v1* layer = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional = nullptr;
    hyprland_focus_grab_v1* grab = nullptr;
    wl_callback* frame = nullptr;
    std::vector<std::uint32_t> outputs;
    std::vector<Element> elements;
    int width = 640, height = 480, pending_width = 640, pending_height = 480;
    double scale = 1;
    int focused = -1, pressed = -1;
    std::uint32_t press_serial = 0, press_seat = 0;
    bool press_touch = false;
    bool closed = false, configured = false, dirty = true, layout_dirty = true, waiting_buffer = false;
    bool configure_pending = false, close_pending = false, grab_pending = false;
    int on_configure = -2, on_close = -2, on_grab_cleared = -2, on_input = -2;
    std::size_t content_bytes = 0;
};

struct Buffer {
    Gui* gui = nullptr;
    std::uint64_t surface = 0;
    wl_buffer* native = nullptr;
    cairo_surface_t* cairo = nullptr;
    void* mapping = nullptr;
    std::size_t bytes = 0;
    int width = 0, height = 0;
    bool busy = false;
    ~Buffer();
};

struct Output {
    Gui* gui = nullptr;
    wl_output* native = nullptr;
    std::uint32_t id = 0;
    std::string name, description;
    int scale = 1;
};

struct Offer {
    wl_data_offer* native = nullptr;
    const char* mime = nullptr;
};

struct Seat {
    Gui* gui = nullptr;
    std::uint32_t id = 0;
    wl_seat* native = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_touch* touch = nullptr;
    wp_cursor_shape_device_v1* cursor_shape = nullptr;
    zwp_text_input_v3* ime = nullptr;
    wl_data_device* data_device = nullptr;
    wl_data_source* source = nullptr;
    std::string clipboard;
    std::vector<std::unique_ptr<Offer>> offers;
    Offer* selection = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* keys = nullptr;
    xkb_compose_state* compose = nullptr;
    std::uint64_t pointer_surface = 0, keyboard_surface = 0, ime_surface = 0, touch_surface = 0;
    std::uint64_t ime_revision = 0, ime_edit = 0, ime_applied_edit = 0;
    std::array<int, 4> ime_cursor{};
    int ime_element = -1;
    std::uint32_t ime_serial = 0, repeat_key = 0, serial = 0;
    double x = 0, y = 0, touch_x = 0, touch_y = 0;
    int repeat_rate = 0, repeat_delay = 400;
    int touch_id = -1;
    std::optional<std::chrono::steady_clock::time_point> repeat_at;
    bool ime_enabled = false, ime_blocked = false, ime_surrounding = false, preedit_set = false, commit_set = false;
    std::string preedit, commit;
    int preedit_cursor = 0, preedit_end = 0;
    std::uint32_t delete_before = 0, delete_after = 0;
};

struct Input {
    enum class Type { motion, leave, button, axis, key, focus, blur, ime, ime_leave } type = Type::motion;
    std::uint64_t surface = 0, revision = 0;
    std::uint32_t seat = 0, serial = 0, key = 0;
    double x = 0, y = 0, delta = 0;
    bool pressed = false, shift = false, ctrl = false, alt = false, touch = false;
    std::string text{}, preedit{};
    int element = -1, preedit_cursor = 0, preedit_end = 0;
    bool commit_set = false, preedit_set = false;
    std::uint32_t before = 0, after = 0;
};

struct Transfer {
    int fd = -1, callback = -2;
    bool writing = false;
    std::string data, error;
    std::size_t offset = 0;
    std::uint64_t surface = 0, revision = 0, edit_revision = 0;
    int element = -1;
    std::chrono::steady_clock::time_point deadline;
};

struct Gui {
    InvocationState& owner;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* shell = nullptr;
    zwlr_layer_shell_v1* layers = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractions = nullptr;
    wp_cursor_shape_manager_v1* cursors = nullptr;
    zwp_text_input_manager_v3* text_input = nullptr;
    wl_data_device_manager* data = nullptr;
    hyprland_focus_grab_manager_v1* focus_grabs = nullptr;
    xkb_context* xkb = nullptr;
    xkb_compose_table* compose = nullptr;
    PangoFontMap* fonts = nullptr;
    PangoContext* text_context = nullptr;
    std::array<std::unique_ptr<Surface>, 32> surfaces;
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Seat>> seats;
    std::vector<Transfer> transfers;
    std::array<Input, 256> events;
    std::size_t event_count = 0, buffer_bytes = 0, wait_offset = static_cast<std::size_t>(-1);
    std::size_t transfer_wait_count = 0;
    std::uint64_t next_surface = 0;
    bool read_prepared = false, outputs_pending = true, stopping = false;
    const char* error = nullptr;
    int on_outputs = -2;
    Seat* event_seat = nullptr;
    std::uint32_t event_serial = 0;
    std::uint64_t event_surface = 0;
    ImageWork images;
    explicit Gui(InvocationState& state);
    ~Gui();
};

void start_gui(InvocationState& state);
void open_gui(lua_State* lua);
void prepare_gui(InvocationState& state, WaitSet& wait);
void read_gui(InvocationState& state, const WaitSet& wait);
int dispatch_gui(InvocationState& state);
void close_gui(Gui& gui) noexcept;

}
