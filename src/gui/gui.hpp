#pragma once

#include "images.hpp"
#include "windows.hpp"
#include <lua.hpp>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/types.h>
#include <wayland-client.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "xdg-shell-client.h"
#include "xdg-output-unstable-v1-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"
#include "viewporter-client.h"
#include "fractional-scale-v1-client.h"
#include "cursor-shape-v1-client.h"
#include "text-input-unstable-v3-client.h"
#include "hyprland-focus-grab-v1-client.h"

namespace hyprworks::lua {

inline constexpr char surface_type[] = "hyprworks.surface";
inline constexpr char element_type[] = "hyprworks.element";
inline constexpr std::size_t max_text = 16384, max_content = 1024 * 1024;
struct Handle { std::uint64_t surface, revision; int element; };

struct InvocationState;
struct WaitSet;
struct Gui;

struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool operator==(const Box&) const = default;
};

enum class Kind { row, column, scroll, box, text, image, button, toggle, slider, input };
/// Array position is valid within one surface revision. Moving a preserved
/// element transfers its editing, image and callback ownership at commit.
struct Element {
    std::string id, text, placeholder, font = "Sans 12";
    std::string accessible_role, accessible_name, accessible_description;
    double width = -1, height = -1, grow = 0, padding = 0, gap = 0, radius = 0;
    double x = 0, y = 0, border_width = 0;
    double value = 0, scroll = 0, scroll_limit = 0;
    std::optional<double> scroll_origin;
    Box box, clip;
    double intrinsic_w = 0, intrinsic_h = 0, measured_width = -1, measured_height = 0;
    PangoLayout* layout = nullptr;
    std::shared_ptr<Image> image;
    std::uint64_t edit_revision = 0;
    std::string preedit;
    Kind kind = Kind::box;
    int parent = -1, end = 0;
    int halign = 0, valign = 0, align = 0;
    std::uint32_t background = 0, color = 0xffffffff, accent = 0x8ab4f8ff, border_color = 0;
    int callback = -2, text_width = -1;
    int natural_text_width = 0, natural_text_height = 0;
    int cursor = 0, anchor = 0;
    std::uint32_t preedit_seat = 0;
    int preedit_cursor = 0, preedit_end = 0;
    bool accessible_hidden = false;
    bool enabled = true, wrap = false, password = false, horizontal = false;
    bool measure_dirty = true, arrange_dirty = true, text_dirty = true;
    bool image_notified = false;
};

struct PopupPlacement {
    int x = 0, y = 0, width = 640, height = 480;
};

struct SurfaceFocus {
    bool focused = false, within = false;
    bool operator==(const SurfaceFocus&) const = default;
};

/// Owns the element tree and interaction state. Lua handles resolve identity
/// and revision; closed slots are reclaimed after callback delivery.
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
    std::uint64_t grab_generation = 0;
    std::uint64_t input_generation = 0;
    wl_callback* frame = nullptr;
    std::uint32_t popup_grab_seat = 0, popup_grab_serial = 0;
    std::vector<std::uint32_t> outputs;
    std::vector<Element> elements;
    std::vector<int> named_elements, image_elements, scroll_elements;
    int width = 640, height = 480, pending_width = 640, pending_height = 480;
    PopupPlacement popup_placement;
    int popup_x = 0, popup_y = 0, pending_popup_x = 0, pending_popup_y = 0;
    std::uint32_t configure_serial = 0;
    std::uint32_t output = 0;
    SurfaceFocus reported_focus;
    double scale = 1;
    int focused = -1, pressed = -1;
    int key_pressed = -1;
    std::uint32_t key_seat = 0, key_code = 0, key_serial = 0;
    bool focus_visible = false;
    std::uint32_t press_serial = 0, press_seat = 0;
    bool press_touch = false;
    bool closed = false, configured = false, dirty = true, layout_dirty = true, waiting_buffer = false;
    bool configure_pending = false, close_pending = false, grab_pending = false;
    bool mapped = false, popup_reposition_pending = false;
    bool dismiss_focus_loss = false, dismiss_escape = false, dismiss_grab_cleared = false, focus_seen = false;
    const char* close_reason = nullptr;
    int on_configure = -2, on_close = -2, on_grab_cleared = -2, on_input = -2, on_focus_change = -2;
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
    Box damage;
    ~Buffer();
};

struct OutputState {
    std::string name, description, make, model;
    int scale = 1, transform = 0, pixel_width = 0, pixel_height = 0, refresh_mhz = 0;
    int physical_width = 0, physical_height = 0;
    int x = 0, y = 0, width = 0, height = 0;
    bool logical = false;
    bool operator==(const OutputState&) const = default;
};

struct Output {
    Gui* gui = nullptr;
    wl_output* native = nullptr;
    zxdg_output_v1* logical = nullptr;
    std::uint32_t id = 0;
    OutputState state, pending;
    bool ready = false;
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
    std::uint64_t keyboard_generation = 0;
    std::array<std::uint64_t, 32> keyboard_tree{};
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
    std::uint64_t surface = 0, revision = 0, generation = 0, keyboard_generation = 0;
    std::uint32_t seat = 0, serial = 0, key = 0, code = 0;
    double x = 0, y = 0, delta = 0;
    bool pressed = false, shift = false, ctrl = false, alt = false, touch = false, horizontal = false, repeated = false;
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
    zxdg_output_manager_v1* output_manager = nullptr;
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
    std::unique_ptr<WindowCatalog> windows;
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Seat>> seats;
    std::vector<Transfer> transfers;
    std::array<Input, 256> events;
    std::size_t event_count = 0, buffer_bytes = 0, wait_offset = static_cast<std::size_t>(-1);
    std::size_t transfer_wait_count = 0;
    std::uint64_t next_surface = 0;
    bool read_prepared = false, outputs_pending = true, focus_pending = false, stopping = false;
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

template<class Function>
void protocol(Gui& gui, Function&& function) noexcept {
    if (gui.error || gui.stopping) return;
    try { function(); }
    catch (...) { gui.error = "cannot retain Wayland event"; }
}

void dirty(Surface& surface);
void damage(Surface& surface, Box box);
void invalidate_element(Surface& surface, int index, bool geometry = false);

inline bool focusable(const Element& element) {
    return element.enabled && (element.kind == Kind::button || element.kind == Kind::toggle ||
        element.kind == Kind::slider || element.kind == Kind::input);
}

inline bool valid_text(const std::string& text) {
    return text.size() <= max_text && g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}

Surface* find_surface(Gui& gui, std::uint64_t id);
Gui& gui_state(lua_State* lua);
void push_handle(lua_State* lua, Handle value, const char* type);
void focus(Surface& surface, int index);
void close_surface(Surface& surface);
void request_surface_close(Surface& surface, const char* reason = "compositor");
void cancel_surface_input(Surface& surface, bool descendants = false);
SurfaceFocus surface_focus(const Surface& surface);
int dispatch_close(Surface& surface);
void destroy_surface(Surface& surface);
void reposition_popup(Surface& surface);
void initialize_popup(Surface& surface);
void update_scale(Surface& surface);
int notify(Gui& gui, std::uint64_t id, int element_index, int reference,
    const char* kind = nullptr, const std::string* error = nullptr);
void register_surfaces(lua_State* lua);

void layout(Surface& surface);
void reveal_focus(Surface& surface, bool native);
int layout_index(const Element& element, int index);
PangoRectangle input_cursor(Element& element, bool focused);
void render(Surface& surface);
Box paint_buffer(Surface& surface, Buffer& buffer);

extern const wl_seat_listener seat_listener;
extern const wl_keyboard_listener keyboard_listener;
void enqueue(Seat& seat, Input event);
void attach_seat_services(Seat& seat);
void destroy_seat(Seat& seat);
void update_ime(Seat& seat);
int process_input(Gui& gui, Input& event);
int dispatch_input(Gui& gui);
int dispatch_scroll(Surface& surface);
bool replace_text(Surface& surface, Element& element, int begin, int end, const std::string& inserted);
void paste(Seat& seat, Surface* surface, int callback = -2);
ssize_t write_pipe(int fd, const char* data, std::size_t size);
void transfer_io(Transfer& transfer, short events);
void register_input(lua_State* lua);

}
