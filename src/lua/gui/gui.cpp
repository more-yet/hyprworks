#include "../invocation.hpp"
#include "gui.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace hyprworks::lua {
namespace {

constexpr char surface_type[] = "hyprworks.surface";
constexpr char element_type[] = "hyprworks.element";
constexpr std::size_t max_buffer_bytes = 256 * 1024 * 1024;
constexpr std::size_t max_text = 16384, max_content = 1024 * 1024;
using Clock = std::chrono::steady_clock;

struct Handle { std::uint64_t surface, revision; int element; };

Surface* find_surface(Gui& gui, std::uint64_t id) {
    for (auto& surface : gui.surfaces)
        if (surface && surface->id == id && !surface->closed) return surface.get();
    return nullptr;
}

Seat* find_seat(Gui& gui, std::uint32_t id) {
    for (auto& seat : gui.seats) if (seat->id == id) return seat.get();
    return nullptr;
}

std::uint64_t surface_id(wl_surface* native) {
    auto* surface = native ? static_cast<Surface*>(wl_surface_get_user_data(native)) : nullptr;
    return surface ? surface->id : 0;
}

template<class Function>
void protocol(Gui& gui, Function&& function) noexcept {
    if (gui.error || gui.stopping) return;
    try { function(); }
    catch (...) { gui.error = "cannot retain Wayland event"; }
}

void enqueue(Seat& seat, Input event) {
    auto& gui = *seat.gui;
    if (gui.event_count == gui.events.size()) {
        gui.error = "Wayland input queue limit reached";
        return;
    }
    event.seat = seat.id;
    gui.events[gui.event_count++] = std::move(event);
}

void dirty(Surface& surface) { surface.dirty = surface.layout_dirty = true; }

void focus(Surface& surface, int index) {
    if (surface.focused == index) return;
    if (surface.focused >= 0) {
        auto& previous = surface.elements[static_cast<std::size_t>(surface.focused)];
        previous.preedit.clear(); ++previous.edit_revision;
    }
    surface.focused = index;
    if (index >= 0) ++surface.elements[static_cast<std::size_t>(index)].edit_revision;
    for (auto& seat : surface.gui->seats)
        if (seat->keyboard_surface == surface.id && seat->compose) xkb_compose_state_reset(seat->compose);
    dirty(surface);
}

void clear_elements(lua_State* lua, std::vector<Element>& elements) {
    for (auto& element : elements) {
        if (element.layout) g_object_unref(element.layout);
        unref(lua, element.callback);
    }
    elements.clear();
}

void destroy_surface(Surface& surface) {
    auto& gui = *surface.gui;
    for (auto& child : gui.surfaces)
        if (child && child->parent == surface.id) destroy_surface(*child);
    surface.parent = 0;
    surface.closed = true;
    if (surface.frame) wl_callback_destroy(std::exchange(surface.frame, nullptr));
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    if (surface.popup) xdg_popup_destroy(std::exchange(surface.popup, nullptr));
    if (surface.toplevel) xdg_toplevel_destroy(std::exchange(surface.toplevel, nullptr));
    if (surface.xdg) xdg_surface_destroy(std::exchange(surface.xdg, nullptr));
    if (surface.layer) zwlr_layer_surface_v1_destroy(std::exchange(surface.layer, nullptr));
    if (surface.fractional) wp_fractional_scale_v1_destroy(std::exchange(surface.fractional, nullptr));
    if (surface.viewport) wp_viewport_destroy(std::exchange(surface.viewport, nullptr));
    if (surface.native) wl_surface_destroy(std::exchange(surface.native, nullptr));
    clear_elements(gui.owner.lua, surface.elements);
    unref(gui.owner.lua, surface.on_configure);
    unref(gui.owner.lua, surface.on_close);
    unref(gui.owner.lua, surface.on_grab_cleared);
    unref(gui.owner.lua, surface.on_input);
}

void close_surface(Surface& surface) {
    surface.closed = true;
    for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id) close_surface(*child);
}

void set_configure(Surface& surface, int width, int height) {
    if (width < 1 || height < 1 || width > 16384 || height > 16384) {
        surface.gui->error = "configured surface dimensions outside 1..16384";
        return;
    }
    surface.width = width; surface.height = height;
    surface.configured = true; surface.configure_pending = true;
    dirty(surface);
}

const xdg_wm_base_listener shell_listener{
    .ping = [](void*, xdg_wm_base* shell, std::uint32_t serial) { xdg_wm_base_pong(shell, serial); }
};
const xdg_surface_listener xdg_listener{
    .configure = [](void* data, xdg_surface* xdg, std::uint32_t serial) {
        auto& surface = *static_cast<Surface*>(data);
        xdg_surface_ack_configure(xdg, serial);
        set_configure(surface, surface.pending_width, surface.pending_height);
    }
};
const xdg_toplevel_listener toplevel_listener{
    .configure = [](void* data, xdg_toplevel*, int width, int height, wl_array*) {
        auto& surface = *static_cast<Surface*>(data);
        if (width > 0) surface.pending_width = width;
        if (height > 0) surface.pending_height = height;
    },
    .close = [](void* data, xdg_toplevel*) { static_cast<Surface*>(data)->close_pending = true; },
    .configure_bounds = [](void*, xdg_toplevel*, int, int) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};
const xdg_popup_listener popup_listener{
    .configure = [](void* data, xdg_popup*, int, int, int width, int height) {
        auto& surface = *static_cast<Surface*>(data);
        surface.pending_width = width; surface.pending_height = height;
    },
    .popup_done = [](void* data, xdg_popup*) { static_cast<Surface*>(data)->close_pending = true; },
    .repositioned = [](void*, xdg_popup*, std::uint32_t) {},
};
const zwlr_layer_surface_v1_listener layer_listener{
    .configure = [](void* data, zwlr_layer_surface_v1* layer, std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
        auto& surface = *static_cast<Surface*>(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        if (width > 16384 || height > 16384) { surface.gui->error = "layer dimensions exceed 16384"; return; }
        set_configure(surface, width ? static_cast<int>(width) : surface.pending_width,
            height ? static_cast<int>(height) : surface.pending_height);
    },
    .closed = [](void* data, zwlr_layer_surface_v1*) { static_cast<Surface*>(data)->close_pending = true; }
};

void update_scale(Surface& surface) {
    if (surface.fractional) return;
    int scale = 1;
    for (const auto& output : surface.gui->outputs)
        if (std::ranges::find(surface.outputs, output->id) != surface.outputs.end()) scale = std::max(scale, output->scale);
    if (surface.scale != scale) { surface.scale = scale; dirty(surface); surface.configure_pending = true; }
}

const wl_surface_listener surface_listener{
    .enter = [](void* data, wl_surface*, wl_output* native) {
        auto& surface = *static_cast<Surface*>(data);
        protocol(*surface.gui, [&] {
            for (const auto& output : surface.gui->outputs)
                if (output->native == native && std::ranges::find(surface.outputs, output->id) == surface.outputs.end())
                    surface.outputs.push_back(output->id);
            update_scale(surface);
        });
    },
    .leave = [](void* data, wl_surface*, wl_output* native) {
        auto& surface = *static_cast<Surface*>(data);
        for (const auto& output : surface.gui->outputs) if (output->native == native) std::erase(surface.outputs, output->id);
        update_scale(surface);
    },
    .preferred_buffer_scale = [](void*, wl_surface*, int) {},
    .preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {},
};
const wp_fractional_scale_v1_listener scale_listener{
    .preferred_scale = [](void* data, wp_fractional_scale_v1*, std::uint32_t scale) {
        auto& surface = *static_cast<Surface*>(data);
        if (!scale || scale > 960) { surface.gui->error = "unsupported fractional scale"; return; }
        surface.scale = scale / 120.0;
        surface.configure_pending = true; dirty(surface);
    }
};
const wl_output_listener output_listener{
    .geometry = [](void*, wl_output*, int, int, int, int, int, const char*, const char*, int) {},
    .mode = [](void*, wl_output*, std::uint32_t, int, int, int) {},
    .done = [](void* data, wl_output*) {
        auto& output = *static_cast<Output*>(data);
        output.gui->outputs_pending = true;
        for (auto& surface : output.gui->surfaces) if (surface) update_scale(*surface);
    },
    .scale = [](void* data, wl_output*, int scale) {
        auto& output = *static_cast<Output*>(data);
        if (scale < 1 || scale > 8) { output.gui->error = "unsupported output scale"; return; }
        output.scale = scale;
    },
    .name = [](void* data, wl_output*, const char* name) {
        auto& output = *static_cast<Output*>(data);
        protocol(*output.gui, [&] { output.name = name; });
    },
    .description = [](void* data, wl_output*, const char* text) {
        auto& output = *static_cast<Output*>(data);
        protocol(*output.gui, [&] { output.description = text; });
    },
};

Input key_event(Seat& seat, std::uint32_t key, bool pressed, std::uint32_t serial) {
    Input event;
    event.type = Input::Type::key; event.surface = seat.keyboard_surface;
    event.serial = serial; event.pressed = pressed;
    if (!seat.keys) return event;
    event.key = xkb_state_key_get_one_sym(seat.keys, key + 8);
    event.shift = xkb_state_mod_name_is_active(seat.keys, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    event.ctrl = xkb_state_mod_name_is_active(seat.keys, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    event.alt = xkb_state_mod_name_is_active(seat.keys, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    if (!pressed || event.ctrl || event.alt) return event;
    char text[128]{};
    if (seat.compose) {
        xkb_compose_state_feed(seat.compose, event.key);
        const auto status = xkb_compose_state_get_status(seat.compose);
        if (status == XKB_COMPOSE_COMPOSING) return event;
        if (status == XKB_COMPOSE_COMPOSED) {
            xkb_compose_state_get_utf8(seat.compose, text, sizeof(text));
            xkb_compose_state_reset(seat.compose);
        } else {
            if (status == XKB_COMPOSE_CANCELLED) xkb_compose_state_reset(seat.compose);
            else xkb_state_key_get_utf8(seat.keys, key + 8, text, sizeof(text));
        }
    } else xkb_state_key_get_utf8(seat.keys, key + 8, text, sizeof(text));
    if (static_cast<unsigned char>(text[0]) >= 32 && text[0] != 127) event.text = text;
    return event;
}

const wl_keyboard_listener keyboard_listener{
    .keymap = [](void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size) {
        auto& seat = *static_cast<Seat*>(data);
        if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
            close(fd);
            if (seat.keys) xkb_state_unref(std::exchange(seat.keys, nullptr));
            if (seat.keymap) xkb_keymap_unref(std::exchange(seat.keymap, nullptr));
            seat.repeat_at.reset();
            if (seat.compose) xkb_compose_state_reset(seat.compose);
            return;
        }
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size || size > 8 * 1024 * 1024) {
            close(fd); seat.gui->error = "unsupported keyboard keymap"; return;
        }
        struct stat status{};
        if (fstat(fd, &status) < 0 || status.st_size < static_cast<off_t>(size)) {
            close(fd); seat.gui->error = "truncated keyboard keymap"; return;
        }
        void* memory = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (memory == MAP_FAILED) { seat.gui->error = "cannot map keyboard keymap"; return; }
        xkb_keymap* map = nullptr;
        if (static_cast<char*>(memory)[size - 1] == '\0')
            map = xkb_keymap_new_from_buffer(seat.gui->xkb, static_cast<char*>(memory), size - 1,
                XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(memory, size);
        if (!map) { seat.gui->error = "invalid keyboard keymap"; return; }
        if (seat.keys) xkb_state_unref(seat.keys);
        if (seat.keymap) xkb_keymap_unref(seat.keymap);
        seat.keymap = map; seat.keys = xkb_state_new(map);
        if (!seat.keys) seat.gui->error = "cannot create keyboard state";
        seat.repeat_at.reset();
        if (seat.compose) xkb_compose_state_reset(seat.compose);
    },
    .enter = [](void* data, wl_keyboard*, std::uint32_t serial, wl_surface* native, wl_array*) {
        auto& seat = *static_cast<Seat*>(data);
        seat.serial = serial; seat.keyboard_surface = surface_id(native);
        enqueue(seat, {.type = Input::Type::focus, .surface = seat.keyboard_surface});
    },
    .leave = [](void* data, wl_keyboard*, std::uint32_t, wl_surface*) {
        auto& seat = *static_cast<Seat*>(data);
        enqueue(seat, {.type = Input::Type::blur, .surface = seat.keyboard_surface});
        seat.keyboard_surface = 0; seat.repeat_at.reset();
        if (seat.compose) xkb_compose_state_reset(seat.compose);
    },
    .key = [](void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
        auto& seat = *static_cast<Seat*>(data); seat.serial = serial;
        protocol(*seat.gui, [&] { enqueue(seat, key_event(seat, key, state == WL_KEYBOARD_KEY_STATE_PRESSED, serial)); });
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED && seat.keymap && seat.repeat_rate > 0 && xkb_keymap_key_repeats(seat.keymap, key + 8)) {
            seat.repeat_key = key; seat.repeat_at = Clock::now() + std::chrono::milliseconds(seat.repeat_delay);
        } else if (key == seat.repeat_key) seat.repeat_at.reset();
    },
    .modifiers = [](void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked, std::uint32_t group) {
        auto& seat = *static_cast<Seat*>(data);
        if (seat.keys) xkb_state_update_mask(seat.keys, depressed, latched, locked, 0, 0, group);
    },
    .repeat_info = [](void* data, wl_keyboard*, int rate, int delay) {
        auto& seat = *static_cast<Seat*>(data);
        seat.repeat_rate = std::clamp(rate, 0, 100); seat.repeat_delay = std::clamp(delay, 0, 5000);
        if (!rate) seat.repeat_at.reset();
    },
};

const wl_pointer_listener pointer_listener{
    .enter = [](void* data, wl_pointer*, std::uint32_t serial, wl_surface* native, wl_fixed_t x, wl_fixed_t y) {
        auto& seat = *static_cast<Seat*>(data);
        seat.pointer_surface = surface_id(native); seat.x = wl_fixed_to_double(x); seat.y = wl_fixed_to_double(y);
        if (seat.cursor_shape) wp_cursor_shape_device_v1_set_shape(seat.cursor_shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        enqueue(seat, {.type = Input::Type::motion, .surface = seat.pointer_surface, .x = seat.x, .y = seat.y});
    },
    .leave = [](void* data, wl_pointer*, std::uint32_t, wl_surface*) {
        auto& seat = *static_cast<Seat*>(data);
        enqueue(seat, {.type = Input::Type::leave, .surface = seat.pointer_surface});
        seat.pointer_surface = 0;
    },
    .motion = [](void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto& seat = *static_cast<Seat*>(data); seat.x = wl_fixed_to_double(x); seat.y = wl_fixed_to_double(y);
        enqueue(seat, {.type = Input::Type::motion, .surface = seat.pointer_surface, .x = seat.x, .y = seat.y});
    },
    .button = [](void* data, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t button, std::uint32_t state) {
        auto& seat = *static_cast<Seat*>(data); seat.serial = serial;
        enqueue(seat, {.type = Input::Type::button, .surface = seat.pointer_surface, .serial = serial,
            .key = button, .x = seat.x, .y = seat.y, .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED});
    },
    .axis = [](void* data, wl_pointer*, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
        auto& seat = *static_cast<Seat*>(data);
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            enqueue(seat, {.type = Input::Type::axis, .surface = seat.pointer_surface, .x = seat.x, .y = seat.y, .delta = wl_fixed_to_double(value)});
    },
    .frame = [](void*, wl_pointer*) {},
    .axis_source = [](void*, wl_pointer*, std::uint32_t) {},
    .axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
    .axis_discrete = [](void*, wl_pointer*, std::uint32_t, int) {},
    .axis_value120 = [](void*, wl_pointer*, std::uint32_t, int) {},
    .axis_relative_direction = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
#ifdef WL_POINTER_WARP_SINCE_VERSION
    .warp = nullptr,
#endif
};

const wl_touch_listener touch_listener{
    .down = [](void* data, wl_touch*, std::uint32_t serial, std::uint32_t, wl_surface* native, int id, wl_fixed_t x, wl_fixed_t y) {
        auto& seat = *static_cast<Seat*>(data);
        if (seat.touch_id >= 0) return;
        seat.touch_id = id; seat.touch_surface = surface_id(native); seat.serial = serial;
        seat.touch_x = wl_fixed_to_double(x); seat.touch_y = wl_fixed_to_double(y);
        enqueue(seat, {.type = Input::Type::button, .surface = seat.touch_surface, .serial = serial,
            .key = BTN_LEFT, .x = seat.touch_x, .y = seat.touch_y, .pressed = true, .touch = true});
    },
    .up = [](void* data, wl_touch*, std::uint32_t serial, std::uint32_t, int id) {
        auto& seat = *static_cast<Seat*>(data);
        if (seat.touch_id != id) return;
        seat.serial = serial;
        enqueue(seat, {.type = Input::Type::button, .surface = seat.touch_surface, .serial = serial,
            .key = BTN_LEFT, .x = seat.touch_x, .y = seat.touch_y, .touch = true});
        seat.touch_id = -1;
    },
    .motion = [](void* data, wl_touch*, std::uint32_t, int id, wl_fixed_t x, wl_fixed_t y) {
        auto& seat = *static_cast<Seat*>(data);
        if (seat.touch_id != id) return;
        seat.touch_x = wl_fixed_to_double(x); seat.touch_y = wl_fixed_to_double(y);
        enqueue(seat, {.type = Input::Type::motion, .surface = seat.touch_surface, .x = seat.touch_x, .y = seat.touch_y, .touch = true});
    },
    .frame = [](void*, wl_touch*) {},
    .cancel = [](void* data, wl_touch*) {
        auto& seat = *static_cast<Seat*>(data); seat.touch_id = -1;
        enqueue(seat, {.type = Input::Type::leave, .surface = seat.touch_surface, .touch = true});
    },
    .shape = [](void*, wl_touch*, int, wl_fixed_t, wl_fixed_t) {},
    .orientation = [](void*, wl_touch*, int, wl_fixed_t) {},
};

const wl_seat_listener seat_listener{
    .capabilities = [](void* data, wl_seat*, std::uint32_t capabilities) {
        auto& seat = *static_cast<Seat*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !seat.pointer) {
            seat.pointer = wl_seat_get_pointer(seat.native);
            if (!seat.pointer) { seat.gui->error = "cannot create pointer"; return; }
            wl_pointer_add_listener(seat.pointer, &pointer_listener, &seat);
            if (seat.gui->cursors) seat.cursor_shape = wp_cursor_shape_manager_v1_get_pointer(seat.gui->cursors, seat.pointer);
        } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && seat.pointer) {
            if (seat.cursor_shape) wp_cursor_shape_device_v1_destroy(std::exchange(seat.cursor_shape, nullptr));
            wl_pointer_release(std::exchange(seat.pointer, nullptr));
            enqueue(seat, {.type = Input::Type::leave, .surface = seat.pointer_surface});
            seat.pointer_surface = 0;
        }
        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !seat.keyboard) {
            seat.keyboard = wl_seat_get_keyboard(seat.native);
            if (!seat.keyboard) { seat.gui->error = "cannot create keyboard"; return; }
            wl_keyboard_add_listener(seat.keyboard, &keyboard_listener, &seat);
        } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && seat.keyboard) {
            wl_keyboard_release(std::exchange(seat.keyboard, nullptr));
            enqueue(seat, {.type = Input::Type::blur, .surface = seat.keyboard_surface});
            seat.keyboard_surface = 0; seat.repeat_at.reset();
            if (seat.compose) xkb_compose_state_reset(seat.compose);
        }
        if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !seat.touch) {
            seat.touch = wl_seat_get_touch(seat.native);
            if (!seat.touch) { seat.gui->error = "cannot create touch input"; return; }
            wl_touch_add_listener(seat.touch, &touch_listener, &seat);
        } else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH) && seat.touch) {
            enqueue(seat, {.type = Input::Type::leave, .surface = seat.touch_surface, .touch = true});
            wl_touch_release(std::exchange(seat.touch, nullptr)); seat.touch_id = -1;
        }
    },
    .name = [](void*, wl_seat*, const char*) {},
};

const zwp_text_input_v3_listener ime_listener{
    .enter = [](void* data, zwp_text_input_v3*, wl_surface* native) {
        auto& seat = *static_cast<Seat*>(data); seat.ime_surface = surface_id(native);
    },
    .leave = [](void* data, zwp_text_input_v3* ime, wl_surface*) {
        auto& seat = *static_cast<Seat*>(data);
        if (seat.ime_enabled) {
            enqueue(seat, {.type = Input::Type::ime_leave, .surface = seat.ime_surface,
                .revision = seat.ime_revision, .element = seat.ime_element});
            zwp_text_input_v3_disable(ime); zwp_text_input_v3_commit(ime); ++seat.ime_serial;
        }
        seat.ime_enabled = false; seat.ime_surface = 0; seat.ime_element = -1;
        seat.preedit.clear(); seat.commit.clear(); seat.commit_set = seat.preedit_set = false;
        seat.delete_before = seat.delete_after = 0;
    },
    .preedit_string = [](void* data, zwp_text_input_v3*, const char* text, int begin, int end) {
        auto& seat = *static_cast<Seat*>(data);
        protocol(*seat.gui, [&] {
            if (text && std::strlen(text) > max_text) { seat.gui->error = "IME preedit exceeds 16 KiB"; return; }
            seat.preedit = text ? text : ""; seat.preedit_cursor = begin; seat.preedit_end = end; seat.preedit_set = true;
        });
    },
    .commit_string = [](void* data, zwp_text_input_v3*, const char* text) {
        auto& seat = *static_cast<Seat*>(data);
        protocol(*seat.gui, [&] {
            if (text && std::strlen(text) > max_text) { seat.gui->error = "IME commit exceeds 16 KiB"; return; }
            seat.commit = text ? text : ""; seat.commit_set = true;
        });
    },
    .delete_surrounding_text = [](void* data, zwp_text_input_v3*, std::uint32_t before, std::uint32_t after) {
        auto& seat = *static_cast<Seat*>(data); seat.delete_before = before; seat.delete_after = after;
    },
    .done = [](void* data, zwp_text_input_v3*, std::uint32_t serial) {
        auto& seat = *static_cast<Seat*>(data);
        protocol(*seat.gui, [&] {
            if (seat.ime_enabled) {
                Input event{.type = Input::Type::ime, .surface = seat.ime_surface, .revision = seat.ime_revision};
                event.serial = serial;
                event.element = seat.ime_element;
                event.text = std::move(seat.commit); event.preedit = std::move(seat.preedit);
                event.commit_set = seat.commit_set; event.preedit_set = seat.preedit_set;
                event.preedit_cursor = seat.preedit_cursor;
                event.preedit_end = seat.preedit_end;
                event.before = seat.delete_before; event.after = seat.delete_after;
                enqueue(seat, std::move(event));
            }
            seat.preedit.clear(); seat.commit.clear(); seat.preedit_set = seat.commit_set = false;
            seat.delete_before = seat.delete_after = 0;
        });
    },
#ifdef ZWP_TEXT_INPUT_V3_ACTION_SINCE_VERSION
    .action = nullptr,
    .language = nullptr,
    .preedit_hint = nullptr,
#endif
};

const wl_data_offer_listener offer_listener{
    .offer = [](void* data, wl_data_offer*, const char* mime) {
        auto& offer = *static_cast<Offer*>(data);
        if (!std::strcmp(mime, "text/plain;charset=utf-8")) offer.mime = "text/plain;charset=utf-8";
        else if (!offer.mime && !std::strcmp(mime, "text/plain")) offer.mime = "text/plain";
    },
    .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
    .action = [](void*, wl_data_offer*, std::uint32_t) {},
};
const wl_data_device_listener data_listener{
    .data_offer = [](void* data, wl_data_device*, wl_data_offer* native) {
        auto& seat = *static_cast<Seat*>(data);
        protocol(*seat.gui, [&] {
            if (seat.offers.size() >= 64) { seat.gui->error = "clipboard offer limit reached"; return; }
            auto offer = std::make_unique<Offer>(); offer->native = native;
            wl_data_offer_add_listener(native, &offer_listener, offer.get());
            seat.offers.push_back(std::move(offer));
            native = nullptr;
        });
        if (native) wl_data_offer_destroy(native);
    },
    .enter = [](void* data, wl_data_device*, std::uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* native) {
        auto& seat = *static_cast<Seat*>(data);
        if (native) wl_data_offer_accept(native, serial, nullptr);
        for (auto it = seat.offers.begin(); it != seat.offers.end(); ++it)
            if ((*it)->native == native && it->get() != seat.selection) {
                wl_data_offer_destroy(native); seat.offers.erase(it); break;
            }
    },
    .leave = [](void*, wl_data_device*) {},
    .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
    .drop = [](void*, wl_data_device*) {},
    .selection = [](void* data, wl_data_device*, wl_data_offer* native) {
        auto& seat = *static_cast<Seat*>(data);
        seat.selection = nullptr;
        std::erase_if(seat.offers, [&](auto& offer) {
            if (offer->native == native) { seat.selection = offer.get(); return false; }
            wl_data_offer_destroy(offer->native); return true;
        });
    },
};

const wl_data_source_listener source_listener{
    .target = [](void*, wl_data_source*, const char*) {},
    .send = [](void* data, wl_data_source*, const char* mime, int fd) {
        auto& seat = *static_cast<Seat*>(data);
        if ((std::strcmp(mime, "text/plain;charset=utf-8") && std::strcmp(mime, "text/plain")) || seat.gui->transfers.size() >= 16) { close(fd); return; }
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return; }
        protocol(*seat.gui, [&] {
            Transfer transfer; transfer.writing = true; transfer.data = seat.clipboard;
            transfer.deadline = Clock::now() + std::chrono::seconds(5);
            seat.gui->transfers.push_back(std::move(transfer));
            seat.gui->transfers.back().fd = fd; fd = -1;
        });
        if (fd >= 0) close(fd);
    },
    .cancelled = [](void* data, wl_data_source* source) {
        auto& seat = *static_cast<Seat*>(data);
        wl_data_source_destroy(source); if (seat.source == source) seat.source = nullptr;
    },
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished = [](void*, wl_data_source*) {},
    .action = [](void*, wl_data_source*, std::uint32_t) {},
};

void attach_seat_services(Seat& seat) {
    if (seat.gui->text_input && !seat.ime) {
        seat.ime = zwp_text_input_manager_v3_get_text_input(seat.gui->text_input, seat.native);
        if (!seat.ime) throw std::runtime_error("cannot create text input");
        zwp_text_input_v3_add_listener(seat.ime, &ime_listener, &seat);
    }
    if (seat.gui->data && !seat.data_device) {
        seat.data_device = wl_data_device_manager_get_data_device(seat.gui->data, seat.native);
        if (!seat.data_device) throw std::runtime_error("cannot create clipboard device");
        wl_data_device_add_listener(seat.data_device, &data_listener, &seat);
    }
    if (seat.gui->cursors && seat.pointer && !seat.cursor_shape)
        seat.cursor_shape = wp_cursor_shape_manager_v1_get_pointer(seat.gui->cursors, seat.pointer);
}

void destroy_seat(Seat& seat) {
    if (seat.source) wl_data_source_destroy(seat.source);
    for (auto& offer : seat.offers) wl_data_offer_destroy(offer->native);
    if (seat.data_device) wl_data_device_release(seat.data_device);
    if (seat.ime) zwp_text_input_v3_destroy(seat.ime);
    if (seat.cursor_shape) wp_cursor_shape_device_v1_destroy(seat.cursor_shape);
    if (seat.pointer) wl_pointer_release(seat.pointer);
    if (seat.keyboard) wl_keyboard_release(seat.keyboard);
    if (seat.touch) wl_touch_release(seat.touch);
    if (seat.keys) xkb_state_unref(seat.keys);
    if (seat.keymap) xkb_keymap_unref(seat.keymap);
    if (seat.compose) xkb_compose_state_unref(seat.compose);
    if (seat.native) wl_seat_release(seat.native);
}

const wl_registry_listener registry_listener{
    .global = [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
        auto& gui = *static_cast<Gui*>(data);
        protocol(gui, [&] {
            const auto bind = [&]<class T>(T*& target, const wl_interface& type, std::uint32_t maximum, std::uint32_t minimum = 1) {
                if (target || std::strcmp(interface, type.name) || version < minimum) return false;
                target = static_cast<T*>(wl_registry_bind(registry, name, &type, std::min(version, maximum)));
                if (!target) throw std::runtime_error("cannot bind Wayland global");
                return true;
            };
            bind(gui.compositor, wl_compositor_interface, 6, 4);
            bind(gui.shm, wl_shm_interface, 1);
            if (bind(gui.shell, xdg_wm_base_interface, 6))
                xdg_wm_base_add_listener(gui.shell, &shell_listener, &gui);
            bind(gui.layers, zwlr_layer_shell_v1_interface, 4, 4);
            bind(gui.viewporter, wp_viewporter_interface, 1);
            bind(gui.fractions, wp_fractional_scale_manager_v1_interface, 1);
            bind(gui.cursors, wp_cursor_shape_manager_v1_interface, 1);
            bind(gui.text_input, zwp_text_input_manager_v3_interface, 1);
            bind(gui.data, wl_data_device_manager_interface, 3, 3);
            bind(gui.focus_grabs, hyprland_focus_grab_manager_v1_interface, 1);
            if (!std::strcmp(interface, wl_output_interface.name) && version >= 4) {
                if (gui.outputs.size() >= 128) throw std::runtime_error("output limit reached");
                auto& output = gui.outputs.emplace_back(std::make_unique<Output>());
                output->gui = &gui; output->id = name;
                output->native = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
                if (!output->native) throw std::runtime_error("cannot bind output");
                wl_output_add_listener(output->native, &output_listener, output.get());
            }
            if (!std::strcmp(interface, wl_seat_interface.name) && version >= 5) {
                if (gui.seats.size() >= 8) throw std::runtime_error("seat limit reached");
                auto& seat = gui.seats.emplace_back(std::make_unique<Seat>());
                seat->gui = &gui; seat->id = name;
                seat->native = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 8u)));
                if (!seat->native) throw std::runtime_error("cannot bind seat");
                if (gui.compose) seat->compose = xkb_compose_state_new(gui.compose, XKB_COMPOSE_STATE_NO_FLAGS);
                wl_seat_add_listener(seat->native, &seat_listener, seat.get());
            }
            for (auto& seat : gui.seats) attach_seat_services(*seat);
        });
    },
    .global_remove = [](void* data, wl_registry*, std::uint32_t name) {
        auto& gui = *static_cast<Gui*>(data);
        std::erase_if(gui.outputs, [&](auto& output) {
            if (output->id != name) return false;
            for (auto& surface : gui.surfaces) if (surface) std::erase(surface->outputs, name);
            wl_output_release(output->native); gui.outputs_pending = true; return true;
        });
        for (auto& surface : gui.surfaces) if (surface) update_scale(*surface);
        std::erase_if(gui.seats, [&](auto& seat) {
            if (seat->id != name) return false;
            enqueue(*seat, {.type = Input::Type::blur, .surface = seat->keyboard_surface});
            enqueue(*seat, {.type = Input::Type::leave, .surface = seat->pointer_surface});
            enqueue(*seat, {.type = Input::Type::leave, .surface = seat->touch_surface, .touch = true});
            enqueue(*seat, {.type = Input::Type::ime_leave, .surface = seat->ime_surface,
                .revision = seat->ime_revision, .element = seat->ime_element});
            destroy_seat(*seat); return true;
        });
    },
};

void check_display(Gui& gui) {
    if (gui.error) throw std::runtime_error(gui.error);
    if (const int error = wl_display_get_error(gui.display)) {
        const wl_interface* interface = nullptr; std::uint32_t id = 0;
        const auto code = wl_display_get_protocol_error(gui.display, &interface, &id);
        throw std::runtime_error(std::string("Wayland: ") + std::strerror(error) +
            (interface ? " (" + std::string(interface->name) + " object " + std::to_string(id) + ", code " + std::to_string(code) + ")" : ""));
    }
}

void synchronize(Gui& gui) {
    bool done = false;
    auto* sync = wl_display_sync(gui.display);
    if (!sync) throw std::runtime_error("cannot synchronize Wayland");
    const wl_callback_listener listener{
        .done = [](void* data, wl_callback*, std::uint32_t) { *static_cast<bool*>(data) = true; }
    };
    wl_callback_add_listener(sync, &listener, &done);
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    try {
        while (!done) {
            if (wl_display_dispatch_pending(gui.display) < 0) check_display(gui);
            check_display(gui);
            if (done) break;
            if (Clock::now() >= deadline) throw std::runtime_error("Wayland initialization timed out");
            if (wl_display_prepare_read(gui.display) < 0) continue;
            gui.read_prepared = true;
            pollfd fd{wl_display_get_fd(gui.display), POLLIN, 0};
            if (wl_display_flush(gui.display) < 0) {
                if (errno == EAGAIN) fd.events |= POLLOUT;
                else { wl_display_cancel_read(gui.display); gui.read_prepared = false; check_display(gui); }
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            const int result = poll(&fd, 1, static_cast<int>(std::max<std::int64_t>(remaining, 0)));
            if (result > 0 && (fd.revents & POLLIN)) wl_display_read_events(gui.display);
            else wl_display_cancel_read(gui.display);
            gui.read_prepared = false;
            if (result < 0 && errno != EINTR) throw std::runtime_error("poll Wayland initialization failed");
            check_display(gui);
        }
    } catch (...) {
        if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
        wl_callback_destroy(sync); throw;
    }
    wl_callback_destroy(sync);
}

Box intersect(Box a, Box b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.w, b.x + b.w) - x),
        std::max(0.0, std::min(a.y + a.h, b.y + b.h) - y)};
}

bool focusable(const Element& element) {
    return element.enabled && (element.kind == Kind::button || element.kind == Kind::toggle ||
        element.kind == Kind::slider || element.kind == Kind::input);
}

void text_layout(Gui& gui, Element& element, double width) {
    if (!element.layout) {
        element.layout = pango_layout_new(gui.text_context);
        auto* font = pango_font_description_from_string(element.font.c_str());
        pango_layout_set_font_description(element.layout, font); pango_font_description_free(font);
    }
    std::string display;
    if (element.kind == Kind::input && element.text.empty() && element.preedit.empty()) {
        pango_layout_set_text(element.layout, element.placeholder.data(), static_cast<int>(element.placeholder.size()));
    } else if (element.password) {
        display.assign(static_cast<std::size_t>(g_utf8_strlen(element.text.c_str(), -1)), '*');
        pango_layout_set_text(element.layout, display.data(), static_cast<int>(display.size()));
    } else if (element.kind == Kind::input && !element.preedit.empty()) {
        display = element.text;
        display.insert(static_cast<std::size_t>(element.cursor), element.preedit);
        pango_layout_set_text(element.layout, display.data(), static_cast<int>(display.size()));
    } else pango_layout_set_text(element.layout, element.text.data(), static_cast<int>(element.text.size()));
    pango_layout_set_single_paragraph_mode(element.layout, element.kind == Kind::input);
    pango_layout_set_wrap(element.layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(element.layout, static_cast<PangoAlignment>(element.align));
    pango_layout_set_ellipsize(element.layout, element.wrap || element.kind == Kind::input ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
    pango_layout_set_width(element.layout, width < 0 || element.kind == Kind::input ? -1 :
        static_cast<int>(std::clamp(width, 0.0, 16384.0) * PANGO_SCALE));
}

void measure(Surface& surface, int index, double width) {
    auto& elements = surface.elements;
    auto& element = elements[static_cast<std::size_t>(index)];
    element.box.w = std::max(0.0, width);
    const double inner = std::max(0.0, width - element.padding * 2);
    double height = 0;
    if (element.layout) {
        text_layout(*surface.gui, element, inner);
        int w, h; pango_layout_get_pixel_size(element.layout, &w, &h);
        height = h;
    } else if (element.kind == Kind::image) height = element.natural_h;
    else if (element.kind == Kind::slider) height = 24;
    double used = 0, growth = 0; int count = 0;
    const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
    const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        const auto& item = elements[static_cast<std::size_t>(child)];
        if (element.kind == Kind::row) {
            if (item.grow > 0) growth += item.grow;
            else used += item.natural_w;
        }
        ++count;
    }
    const double gaps = element.gap * std::max(0, count - 1);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        auto& item = elements[static_cast<std::size_t>(child)];
        double child_width = item.width >= 0 ? item.width : item.halign ? item.natural_w : inner;
        if (element.kind == Kind::row)
            child_width = item.grow > 0 && growth > 0 ? std::max(0.0, inner - used - gaps) * item.grow / growth : item.natural_w;
        else if (horizontal) child_width = item.natural_w;
        measure(surface, child, child_width);
        if (vertical) height += item.natural_h;
        else height = std::max(height, item.natural_h);
    }
    if (vertical) height += gaps;
    element.natural_h = element.height >= 0 ? element.height : height + 2 * element.padding;
    if (element.kind == Kind::scroll && element.height < 0) element.natural_h = std::min(element.natural_h, 240.0);
}

void arrange(Surface& surface, int index, Box box, Box clip) {
    auto& elements = surface.elements;
    auto& element = elements[static_cast<std::size_t>(index)];
    element.box = box; element.clip = intersect(box, clip);
    const Box inner{box.x + element.padding, box.y + element.padding,
        std::max(0.0, box.w - element.padding * 2), std::max(0.0, box.h - element.padding * 2)};
    double used = 0, growth = 0; int count = 0;
    const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
    const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        const auto& item = elements[static_cast<std::size_t>(child)];
        if (item.grow > 0 && element.kind == Kind::column) growth += item.grow;
        else used += horizontal ? item.box.w : item.natural_h;
        ++count;
    }
    used += element.gap * std::max(0, count - 1);
    if (element.kind == Kind::scroll) element.scroll = std::clamp(element.scroll, 0.0, std::max(0.0, used - (horizontal ? inner.w : inner.h)));
    double offset = element.kind == Kind::scroll ? -element.scroll : 0;
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        auto& item = elements[static_cast<std::size_t>(child)];
        Box child_box{inner.x, inner.y, item.box.w, item.height >= 0 ? item.height : item.valign ? item.natural_h : inner.h};
        if (horizontal) { child_box.x += offset; offset += child_box.w + element.gap; }
        else if (vertical) {
            child_box.y += offset;
            child_box.h = item.grow > 0 && growth > 0 ? std::max(0.0, inner.h - used) * item.grow / growth : item.natural_h;
            offset += child_box.h + element.gap;
        }
        if (!horizontal && item.halign >= 2) child_box.x += (inner.w - child_box.w) * (item.halign == 2 ? 0.5 : 1);
        if (!vertical && item.valign >= 2) child_box.y += (inner.h - child_box.h) * (item.valign == 2 ? 0.5 : 1);
        child_box.x += item.x; child_box.y += item.y;
        arrange(surface, child, child_box, intersect(inner, clip));
    }
}

void layout(Surface& surface) {
    if (!surface.layout_dirty || surface.elements.empty()) return;
    auto& elements = surface.elements;
    for (int index = static_cast<int>(elements.size()) - 1; index >= 0; --index) {
        auto& element = elements[static_cast<std::size_t>(index)];
        double width = 0, height = 0; int count = 0;
        const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
        const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
        if (element.kind == Kind::text || element.kind == Kind::button || element.kind == Kind::toggle || element.kind == Kind::input) {
            text_layout(*surface.gui, element, -1);
            int w, h; pango_layout_get_pixel_size(element.layout, &w, &h); width = w; height = h;
            if (element.kind == Kind::input) width = std::max(width, 160.0);
        } else if (element.kind == Kind::image) {
            width = height = 32;
            if (element.image && element.image->ready.load(std::memory_order_acquire) && element.image->error.empty()) {
                width = element.image->width / surface.scale; height = element.image->height / surface.scale;
            }
        } else if (element.kind == Kind::slider) { width = 120; height = 24; }
        for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
            const auto& item = elements[static_cast<std::size_t>(child)];
            if (horizontal) width += item.natural_w; else width = std::max(width, item.natural_w);
            if (vertical) height += item.natural_h; else height = std::max(height, item.natural_h);
            ++count;
        }
        if (horizontal) width += std::max(0, count - 1) * element.gap;
        if (vertical) height += std::max(0, count - 1) * element.gap;
        element.natural_w = element.width >= 0 ? element.width : width + element.padding * 2;
        element.natural_h = element.height >= 0 ? element.height : height + element.padding * 2;
    }
    measure(surface, 0, surface.width);
    arrange(surface, 0, {0, 0, static_cast<double>(surface.width), static_cast<double>(surface.height)},
        {0, 0, static_cast<double>(surface.width), static_cast<double>(surface.height)});
    surface.layout_dirty = false;
}

void color(cairo_t* cr, std::uint32_t rgba) {
    cairo_set_source_rgba(cr, (rgba >> 24) / 255.0, ((rgba >> 16) & 255) / 255.0, ((rgba >> 8) & 255) / 255.0, (rgba & 255) / 255.0);
}

void rounded(cairo_t* cr, Box box, double radius) {
    const double r = std::min({radius, box.w / 2, box.h / 2});
    constexpr double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, box.x + box.w - r, box.y + r, r, -pi / 2, 0);
    cairo_arc(cr, box.x + box.w - r, box.y + box.h - r, r, 0, pi / 2);
    cairo_arc(cr, box.x + r, box.y + box.h - r, r, pi / 2, pi);
    cairo_arc(cr, box.x + r, box.y + r, r, pi, pi * 1.5);
    cairo_close_path(cr);
}

int layout_index(const Element& element, int index) {
    return element.password ? static_cast<int>(g_utf8_pointer_to_offset(element.text.c_str(), element.text.c_str() + index)) : index;
}

PangoRectangle input_cursor(Element& element, bool focused) {
    const int position = element.cursor + (element.preedit_cursor >= 0 ? std::min(element.preedit_cursor, static_cast<int>(element.preedit.size())) : 0);
    PangoRectangle cursor{};
    pango_layout_get_cursor_pos(element.layout, layout_index(element, position), &cursor, nullptr);
    if (focused) {
        const double x = std::max(0.0, cursor.x / double(PANGO_SCALE));
        const double visible = std::max(0.0, element.box.w - element.padding * 2 - 2);
        element.scroll = std::clamp(element.scroll, std::max(0.0, x - visible), x);
    }
    return cursor;
}

void paint(Surface& surface, cairo_t* cr) {
    for (std::size_t index = 0; index < surface.elements.size(); ++index) {
        auto& element = surface.elements[index];
        const auto box = element.box;
        if (element.clip.w <= 0 || element.clip.h <= 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, element.clip.x, element.clip.y, element.clip.w, element.clip.h); cairo_clip(cr);
        color(cr, element.kind == Kind::toggle && element.value != 0 ? element.accent : element.background);
        rounded(cr, box, element.radius); cairo_fill(cr);
        if (element.border_width > 0) {
            const double inset = element.border_width / 2;
            color(cr, element.border_color); cairo_set_line_width(cr, element.border_width);
            rounded(cr, {box.x + inset, box.y + inset, std::max(0.0, box.w - 2 * inset),
                std::max(0.0, box.h - 2 * inset)}, element.radius);
            cairo_stroke(cr);
        }
        if (element.kind == Kind::slider) {
            color(cr, element.accent);
            rounded(cr, {box.x + 4, box.y + box.h / 2 - 2, std::max(0.0, box.w - 8) * element.value, 4}, 2); cairo_fill(cr);
            cairo_arc(cr, box.x + 4 + std::max(0.0, box.w - 8) * element.value, box.y + box.h / 2, 5, 0, 6.283185307); cairo_fill(cr);
        }
        if (element.kind == Kind::image && element.image && element.image->ready.load(std::memory_order_acquire) && element.image->error.empty()) {
            const auto& image = *element.image;
            const double factor = std::min(box.w / image.width, box.h / image.height);
            cairo_save(cr);
            cairo_translate(cr, box.x + (box.w - image.width * factor) / 2, box.y + (box.h - image.height * factor) / 2);
            if (factor > 0) {
                cairo_scale(cr, factor, factor); cairo_set_source_surface(cr, image.surface, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR); cairo_paint(cr);
            }
            cairo_restore(cr);
        }
        if (element.layout) {
            const double x = box.x + element.padding, y = box.y + element.padding;
            cairo_rectangle(cr, x, y, std::max(0.0, box.w - element.padding * 2), std::max(0.0, box.h - element.padding * 2));
            cairo_clip(cr);
            double offset = 0;
            PangoRectangle cursor{};
            if (element.kind == Kind::input) {
                cursor = input_cursor(element, static_cast<int>(index) == surface.focused);
                offset = element.scroll;
                if ((element.cursor != element.anchor && element.preedit.empty()) ||
                    (!element.preedit.empty() && element.preedit_cursor >= 0 && element.preedit_end >= 0 && element.preedit_cursor != element.preedit_end)) {
                    const int begin = element.preedit.empty() ? std::min(element.cursor, element.anchor) : element.cursor + std::min(element.preedit_cursor, element.preedit_end);
                    const int end = element.preedit.empty() ? std::max(element.cursor, element.anchor) : element.cursor + std::max(element.preedit_cursor, element.preedit_end);
                    int* ranges = nullptr, count = 0;
                    auto* line = pango_layout_get_line_readonly(element.layout, 0);
                    pango_layout_line_get_x_ranges(line, layout_index(element, begin), layout_index(element, end), &ranges, &count);
                    color(cr, (element.accent & 0xffffff00) | 100);
                    for (int i = 0; i < count; ++i)
                        cairo_rectangle(cr, x + ranges[i * 2] / double(PANGO_SCALE) - offset, y,
                            (ranges[i * 2 + 1] - ranges[i * 2]) / double(PANGO_SCALE), cursor.height / double(PANGO_SCALE));
                    cairo_fill(cr); g_free(ranges);
                }
            }
            color(cr, element.kind == Kind::input && element.text.empty() && element.preedit.empty() ?
                (element.color & 0xffffff00) | ((element.color & 255) / 2) : element.color);
            cairo_move_to(cr, x - offset, y); pango_cairo_show_layout(cr, element.layout);
            if (element.kind == Kind::input && static_cast<int>(index) == surface.focused) {
                if (element.preedit.empty() || element.preedit_cursor >= 0) {
                    cairo_rectangle(cr, x + cursor.x / double(PANGO_SCALE) - offset, y + cursor.y / double(PANGO_SCALE),
                        1, cursor.height / double(PANGO_SCALE)); cairo_fill(cr);
                }
                if (!element.preedit.empty()) {
                    PangoRectangle start{}, end{};
                    pango_layout_get_cursor_pos(element.layout, element.cursor, &start, nullptr);
                    pango_layout_get_cursor_pos(element.layout, element.cursor + static_cast<int>(element.preedit.size()), &end, nullptr);
                    cairo_rectangle(cr, x + std::min(start.x, end.x) / double(PANGO_SCALE) - offset,
                        y + cursor.height / double(PANGO_SCALE) - 1, std::abs(end.x - start.x) / double(PANGO_SCALE), 1); cairo_fill(cr);
                }
            }
        }
        cairo_restore(cr);
        if (static_cast<int>(index) == surface.focused && focusable(element)) {
            cairo_save(cr);
            cairo_rectangle(cr, element.clip.x, element.clip.y, element.clip.w, element.clip.h); cairo_clip(cr);
            color(cr, element.accent); cairo_set_line_width(cr, 1);
            rounded(cr, {box.x + 0.5, box.y + 0.5, std::max(0.0, box.w - 1), std::max(0.0, box.h - 1)}, element.radius);
            cairo_stroke(cr); cairo_restore(cr);
        }
    }
}

const wl_buffer_listener buffer_listener{
    .release = [](void* data, wl_buffer*) {
        auto& buffer = *static_cast<Buffer*>(data); buffer.busy = false;
        for (auto& surface : buffer.gui->surfaces) if (surface) surface->waiting_buffer = false;
    }
};
const wl_callback_listener frame_listener{
    .done = [](void* data, wl_callback* callback, std::uint32_t) {
        auto& surface = *static_cast<Surface*>(data);
        wl_callback_destroy(callback); surface.frame = nullptr;
    }
};

void render(Surface& surface) {
    if (!surface.configured || surface.closed || !surface.dirty || surface.frame) return;
    auto& gui = *surface.gui;
    const auto width = static_cast<int>(std::ceil(surface.width * surface.scale));
    const auto height = static_cast<int>(std::ceil(surface.height * surface.scale));
    if (width < 1 || height < 1 || width > 16384 || height > 16384) throw std::runtime_error("scaled surface dimensions exceed 16384");
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (bytes > max_buffer_bytes / 2) throw std::runtime_error("surface exceeds 128 MiB buffer limit");
    Buffer* buffer = nullptr;
    std::size_t count = 0;
    std::erase_if(gui.buffers, [&](auto& item) {
        const auto* owner = find_surface(gui, item->surface);
        return !item->busy && (!owner || (item->surface == surface.id && (item->width != width || item->height != height)));
    });
    for (auto& item : gui.buffers) {
        if (item->surface != surface.id) continue;
        ++count;
        if (!item->busy && item->width == width && item->height == height) buffer = item.get();
    }
    if (!buffer) {
        if (count >= 3) { surface.waiting_buffer = true; return; }
        if (bytes > max_buffer_bytes - gui.buffer_bytes) {
            std::erase_if(gui.buffers, [](const auto& item) { return !item->busy; });
            if (bytes > max_buffer_bytes - gui.buffer_bytes) throw std::runtime_error("presentation storage exceeds 256 MiB");
        }
        auto replacement = std::make_unique<Buffer>();
        replacement->gui = &gui; replacement->surface = surface.id;
        replacement->width = width; replacement->height = height;
        const int fd = memfd_create("hyprworks-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) throw std::runtime_error("cannot create presentation buffer");
        if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) { close(fd); throw std::runtime_error("cannot size presentation buffer"); }
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
        replacement->mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (replacement->mapping == MAP_FAILED) { close(fd); throw std::runtime_error("cannot map presentation buffer"); }
        replacement->bytes = bytes; gui.buffer_bytes += bytes;
        auto* pool = wl_shm_create_pool(gui.shm, fd, static_cast<int>(bytes));
        close(fd);
        if (!pool) throw std::runtime_error("cannot create Wayland buffer pool");
        replacement->native = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (!replacement->native) throw std::runtime_error("cannot create Wayland buffer");
        wl_buffer_add_listener(replacement->native, &buffer_listener, replacement.get());
        replacement->cairo = cairo_image_surface_create_for_data(static_cast<unsigned char*>(replacement->mapping), CAIRO_FORMAT_ARGB32, width, height, width * 4);
        if (cairo_surface_status(replacement->cairo) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("cannot create Cairo buffer");
        buffer = replacement.get(); gui.buffers.push_back(std::move(replacement));
    }
    layout(surface);
    const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(cairo_create(buffer->cairo), cairo_destroy);
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr.get(), 0, 0, 0, 0); cairo_paint(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_OVER);
    cairo_scale(cr.get(), static_cast<double>(width) / surface.width, static_cast<double>(height) / surface.height);
    paint(surface, cr.get());
    if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr.get())));
    cairo_surface_flush(buffer->cairo);
    if (surface.viewport) {
        wl_surface_set_buffer_scale(surface.native, 1);
        wp_viewport_set_destination(surface.viewport, surface.width, surface.height);
    } else wl_surface_set_buffer_scale(surface.native, static_cast<int>(surface.scale));
    if (surface.xdg) xdg_surface_set_window_geometry(surface.xdg, 0, 0, surface.width, surface.height);
    surface.frame = wl_surface_frame(surface.native);
    if (!surface.frame) throw std::runtime_error("cannot create frame callback");
    wl_callback_add_listener(surface.frame, &frame_listener, &surface);
    wl_surface_attach(surface.native, buffer->native, 0, 0);
    wl_surface_damage_buffer(surface.native, 0, 0, width, height);
    buffer->busy = true; surface.dirty = false;
    wl_surface_commit(surface.native);
    surface.waiting_buffer = false;
}

Gui& gui_state(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) throw std::runtime_error("runtime is stopping");
    if (!state.gui) throw std::runtime_error("GUI is unavailable in headless mode");
    return *state.gui;
}

Handle handle(lua_State* lua, int index, const char* type) {
    return *static_cast<Handle*>(luaL_checkudata(lua, index, type));
}

Surface& live_surface(Gui& gui, Handle value) {
    auto* surface = find_surface(gui, value.surface);
    if (!surface) throw std::runtime_error("surface is closed");
    if (value.element >= 0 && (value.revision != surface->revision || static_cast<std::size_t>(value.element) >= surface->elements.size()))
        throw std::runtime_error("element belongs to a replaced tree");
    return *surface;
}

void push_handle(lua_State* lua, Handle value, const char* type) {
    *static_cast<Handle*>(lua_newuserdatauv(lua, sizeof(Handle), 0)) = value;
    luaL_setmetatable(lua, type);
}

double number(lua_State* lua, int table, const char* name, double fallback, double minimum, double maximum) {
    field(lua, table, name);
    const double value = luaL_optnumber(lua, -1, fallback);
    if (!std::isfinite(value) || value < minimum || value > maximum) luaL_error(lua, "%s is out of range", name);
    lua_pop(lua, 1); return value;
}

std::uint32_t read_color(lua_State* lua, int table, const char* name, std::uint32_t fallback) {
    field(lua, table, name);
    if (lua_isnil(lua, -1)) { lua_pop(lua, 1); return fallback; }
    std::size_t length;
    const char* text = luaL_checklstring(lua, -1, &length);
    if ((length != 7 && length != 9) || text[0] != '#') luaL_error(lua, "%s must be #RRGGBB or #RRGGBBAA", name);
    std::uint32_t value = 0;
    const auto result = std::from_chars(text + 1, text + length, value, 16);
    if (result.ec != std::errc{} || result.ptr != text + length) luaL_error(lua, "invalid color");
    lua_pop(lua, 1); return length == 7 ? (value << 8) | 255 : value;
}

bool valid_text(const std::string& text) {
    return text.size() <= max_text && text.find('\0') == std::string::npos &&
        g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}

void properties(lua_State* lua, int table, Element& element, Gui& gui) {
    table = lua_absindex(lua, table);
    text_field(lua, table, "id", element.id, 128);
    field(lua, table, "text");
    bool text_changed = false;
    if (!lua_isnil(lua, -1)) {
        std::size_t size; const char* text = luaL_checklstring(lua, -1, &size);
        if (size > max_text) luaL_error(lua, "text exceeds 16 KiB");
        text_changed = element.text != std::string_view(text, size);
        element.text.assign(text, size);
    }
    lua_pop(lua, 1);
    if (!valid_text(element.text)) luaL_error(lua, "text must be valid UTF-8 without NUL");
    if (element.kind == Kind::input) {
        text_field(lua, table, "placeholder", element.placeholder, max_text);
        if (!valid_text(element.placeholder)) luaL_error(lua, "placeholder must be valid UTF-8 without NUL");
    }
    text_field(lua, table, "font", element.font, 256);
    auto* font = pango_font_description_from_string(element.font.c_str());
    const int font_size = pango_font_description_get_size(font);
    pango_font_description_free(font);
    if (font_size < 0 || font_size > 256 * PANGO_SCALE) luaL_error(lua, "font size exceeds 256");
    element.width = number(lua, table, "width", element.width, -1, 16384);
    element.height = number(lua, table, "height", element.height, -1, 16384);
    element.x = number(lua, table, "x", element.x, -16384, 16384);
    element.y = number(lua, table, "y", element.y, -16384, 16384);
    static const char* horizontal_alignments[] = {"stretch", "left", "center", "right", nullptr};
    static const char* vertical_alignments[] = {"stretch", "top", "center", "bottom", nullptr};
    static const char* text_alignments[] = {"left", "center", "right", nullptr};
    element.halign = enum_field(lua, table, "halign", horizontal_alignments[element.halign], horizontal_alignments);
    element.valign = enum_field(lua, table, "valign", vertical_alignments[element.valign], vertical_alignments);
    element.align = enum_field(lua, table, "align", text_alignments[element.align], text_alignments);
    element.grow = number(lua, table, "grow", element.grow, 0, 1024);
    element.padding = number(lua, table, "padding", element.padding, 0, 1024);
    element.gap = number(lua, table, "gap", element.gap, 0, 1024);
    element.radius = number(lua, table, "radius", element.radius, 0, 1024);
    element.border_width = number(lua, table, "border_width", element.border_width, 0, 1024);
    element.border_color = read_color(lua, table, "border_color", element.border_color);
    element.value = number(lua, table, "value", element.value, 0, 1);
    element.enabled = boolean_field(lua, table, "enabled", element.enabled);
    element.wrap = boolean_field(lua, table, "wrap", element.wrap);
    if (element.kind == Kind::input) {
        element.password = boolean_field(lua, table, "password", element.password);
        if (element.password) element.preedit.clear();
    }
    if (element.kind == Kind::scroll) {
        element.scroll = number(lua, table, "scroll", element.scroll, 0, 33554432);
        element.horizontal = boolean_field(lua, table, "horizontal", element.horizontal);
    }
    element.background = read_color(lua, table, "background", element.background);
    element.color = read_color(lua, table, "color", element.color);
    element.accent = read_color(lua, table, "accent", element.accent);
    field(lua, table, element.kind == Kind::button ? "on_activate" : element.kind == Kind::image ? "on_loaded" : "on_change");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TFUNCTION);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        unref(lua, element.callback); element.callback = reference;
    } else lua_pop(lua, 1);
    if (element.kind == Kind::image) {
        field(lua, table, "source");
        if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TTABLE);
            element.image = std::make_shared<Image>(gui.images); element.image_notified = false;
            auto& image = *element.image;
            text_field(lua, -1, "path", image.path, 4096);
            text_field(lua, -1, "icon", image.icon, 256);
            image.theme = "hicolor"; text_field(lua, -1, "theme", image.theme, 256);
            image.pixels = static_cast<int>(integer_field(lua, -1, "pixels", 64, 1, 4096));
            image.width = static_cast<int>(integer_field(lua, -1, "width", 0, 0, 4096));
            image.height = static_cast<int>(integer_field(lua, -1, "height", 0, 0, 4096));
            image.channels = static_cast<int>(integer_field(lua, -1, "channels", 0, 0, 4));
            if (image.channels && image.channels != 3 && image.channels != 4) luaL_error(lua, "raw image channels must be 3 or 4");
            field(lua, -1, "data");
            if (!lua_isnil(lua, -1)) {
                std::size_t length; const char* bytes = luaL_checklstring(lua, -1, &length);
                if (length > 8 * 1024 * 1024) luaL_error(lua, "image input exceeds 8 MiB");
                image.reserve_input(length);
                image.encoded.assign(bytes, length);
            }
            lua_pop(lua, 1);
            if (int(!image.path.empty()) + int(!image.icon.empty()) + int(!image.encoded.empty()) != 1)
                luaL_error(lua, "image source requires exactly one of path, icon or data");
            if (image.channels && (image.width < 1 || image.height < 1 ||
                image.encoded.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * static_cast<std::size_t>(image.channels)))
                luaL_error(lua, "raw image dimensions do not match its data");
        }
        lua_pop(lua, 1);
    }
    if (text_changed) {
        element.cursor = element.anchor = static_cast<int>(element.text.size());
        element.preedit.clear();
    }
}

struct ParsedTree {
    lua_State* lua;
    std::vector<Element> elements;
    ~ParsedTree() { clear_elements(lua, elements); }
};

int parse_tree(lua_State* lua, int table, ParsedTree& tree, Gui& gui, int parent, unsigned depth) {
    if (!lua_checkstack(lua, 12)) luaL_error(lua, "cannot grow Lua parsing stack");
    if (depth > 32 || tree.elements.size() == 2048) luaL_error(lua, "tree limit reached (2048 elements, depth 32)");
    luaL_checktype(lua, table, LUA_TTABLE);
    table = lua_absindex(lua, table);
    static const char* kinds[] = {"row", "column", "overlay", "scroll", "box", "text", "image", "button", "toggle", "slider", "input", nullptr};
    const auto kind = static_cast<Kind>(enum_field(lua, table, "kind", "box", kinds));
    const int index = static_cast<int>(tree.elements.size());
    tree.elements.emplace_back();
    auto& element = tree.elements.back(); element.kind = kind; element.parent = parent;
    if (kind == Kind::button || kind == Kind::toggle || kind == Kind::input) {
        element.padding = 6; element.radius = 4; element.background = 0x303238ff;
    }
    properties(lua, table, element, gui);
    field(lua, table, "children");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        if (kind != Kind::row && kind != Kind::column && kind != Kind::overlay && kind != Kind::scroll && kind != Kind::box && kind != Kind::button)
            luaL_error(lua, "only containers accept children");
        const auto count = lua_rawlen(lua, -1);
        if (count > 2048) luaL_error(lua, "too many children");
        for (std::size_t child = 1; child <= count; ++child) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(child));
            parse_tree(lua, -1, tree, gui, index, depth + 1); lua_pop(lua, 1);
        }
    }
    lua_pop(lua, 1);
    tree.elements[static_cast<std::size_t>(index)].end = static_cast<int>(tree.elements.size());
    return index;
}

std::size_t element_text_bytes(const Element& element) {
    return element.id.size() + element.text.size() + element.placeholder.size() + element.font.size() +
        (element.image ? element.image->icon.size() + element.image->path.size() + element.image->theme.size() : 0);
}

std::size_t content_bytes(const std::vector<Element>& elements) {
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const auto& element = elements[i];
        bytes += element_text_bytes(element);
        if (!element.id.empty()) for (std::size_t j = 0; j < i; ++j)
            if (elements[j].id == element.id) throw std::runtime_error("duplicate element id: " + element.id);
    }
    if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    return bytes;
}

int set_content(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, surface_type);
    live_surface(gui, identity);
    luaL_checktype(lua, 2, LUA_TTABLE);
    ParsedTree parsed{lua, {}};
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) { parse_tree(lua, 2, parsed, gui, -1, 1); return 0; }, 0, 0, 1) < 0) return -1;
    const auto bytes = content_bytes(parsed.elements);
    auto& surface = live_surface(gui, identity);
    focus(surface, -1);
    surface.elements.swap(parsed.elements);
    ++surface.revision; surface.pressed = -1; surface.content_bytes = bytes;
    dirty(surface);
    wake(gui.owner);
    lua_pushboolean(lua, true); return 1;
}

int find_element(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, surface_type);
    std::size_t length; const char* id = luaL_checklstring(lua, 2, &length);
    const auto& surface = live_surface(gui, identity);
    for (std::size_t i = 0; i < surface.elements.size(); ++i) {
        if (surface.elements[i].id != std::string_view(id, length)) continue;
        push_handle(lua, {surface.id, surface.revision, static_cast<int>(i)}, element_type); return 1;
    }
    lua_pushnil(lua); lua_pushliteral(lua, "element id not found"); return 2;
}

int replace_children(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, element_type);
    const auto& initial = live_surface(gui, identity);
    const auto kind = initial.elements[static_cast<std::size_t>(identity.element)].kind;
    if (kind != Kind::row && kind != Kind::column && kind != Kind::overlay &&
        kind != Kind::scroll && kind != Kind::box && kind != Kind::button)
        throw std::runtime_error("only containers accept children");
    unsigned depth = 2;
    for (int parent = initial.elements[static_cast<std::size_t>(identity.element)].parent; parent >= 0;
        parent = initial.elements[static_cast<std::size_t>(parent)].parent) ++depth;
    luaL_checktype(lua, 2, LUA_TTABLE);
    ParsedTree parsed{lua, {}};
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        const auto count = lua_rawlen(lua, 2);
        if (count > 2048) luaL_error(lua, "too many children");
        for (std::size_t i = 1; i <= count; ++i) {
            lua_rawgeti(lua, 2, static_cast<lua_Integer>(i));
            parse_tree(lua, -1, parsed, gui, -1, depth);
            lua_pop(lua, 1);
        }
        return 0;
    }, 0, 0, 1) < 0) return -1;
    auto& surface = live_surface(gui, identity);
    const int begin = identity.element + 1, end = surface.elements[static_cast<std::size_t>(identity.element)].end;
    const int delta = static_cast<int>(parsed.elements.size()) - (end - begin);
    if (static_cast<int>(surface.elements.size()) + delta > 2048) throw std::runtime_error("tree exceeds 2048 elements");
    auto bytes = content_bytes(parsed.elements);
    for (std::size_t i = 0; i < surface.elements.size(); ++i) {
        if (static_cast<int>(i) >= begin && static_cast<int>(i) < end) continue;
        const auto& retained = surface.elements[i];
        bytes += element_text_bytes(retained);
        if (!retained.id.empty()) for (const auto& added : parsed.elements)
            if (retained.id == added.id) throw std::runtime_error("duplicate element id: " + retained.id);
    }
    if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    std::vector<Element> combined;
    combined.reserve(static_cast<std::size_t>(static_cast<int>(surface.elements.size()) + delta));
    const int previous_focus = surface.focused;
    focus(surface, -1);
    for (int i = 0; i < begin; ++i) {
        auto& element = surface.elements[static_cast<std::size_t>(i)];
        if (element.end >= end) element.end += delta;
        combined.push_back(std::move(element));
    }
    for (auto& element : parsed.elements) {
        element.parent = element.parent < 0 ? identity.element : element.parent + begin;
        element.end += begin;
        combined.push_back(std::move(element));
    }
    for (int i = begin; i < end; ++i) {
        auto& element = surface.elements[static_cast<std::size_t>(i)];
        if (element.layout) g_object_unref(element.layout);
        unref(lua, element.callback);
    }
    for (int i = end; i < static_cast<int>(surface.elements.size()); ++i) {
        auto& element = surface.elements[static_cast<std::size_t>(i)];
        if (element.parent >= end) element.parent += delta;
        element.end += delta;
        combined.push_back(std::move(element));
    }
    parsed.elements.clear();
    surface.elements = std::move(combined);
    ++surface.revision; surface.pressed = -1; surface.content_bytes = bytes;
    if (previous_focus >= 0 && (previous_focus < begin || previous_focus >= end))
        focus(surface, previous_focus >= end ? previous_focus + delta : previous_focus);
    dirty(surface); wake(gui.owner);
    lua_pushboolean(lua, true); return 1;
}

int set_element(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, element_type);
    auto& surface = live_surface(gui, identity);
    luaL_checktype(lua, 2, LUA_TTABLE);
    ParsedTree parsed{lua, {}};
    parsed.elements.push_back(surface.elements[static_cast<std::size_t>(identity.element)]);
    auto& replacement = parsed.elements.front();
    replacement.layout = nullptr; replacement.callback = LUA_NOREF;
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        const auto& current = live_surface(gui, identity).elements[static_cast<std::size_t>(identity.element)];
        if (current.callback >= 0) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, current.callback);
            replacement.callback = luaL_ref(lua, LUA_REGISTRYINDEX);
        }
        properties(lua, 2, replacement, gui); return 0;
    }, 0, 0, 1) < 0) return -1;
    auto& live = live_surface(gui, identity);
    auto& element = live.elements[static_cast<std::size_t>(identity.element)];
    if (replacement.id != element.id) throw std::runtime_error("element id is immutable");
    const auto bytes = live.content_bytes - element_text_bytes(element) + element_text_bytes(replacement);
    if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    replacement.edit_revision = element.edit_revision +
        (replacement.text != element.text || replacement.password != element.password ? 1 : 0);
    if (replacement.font == element.font) std::swap(replacement.layout, element.layout);
    std::swap(replacement, element); live.content_bytes = bytes;
    if (!element.enabled) {
        if (live.focused == identity.element) focus(live, -1);
        if (live.pressed == identity.element) live.pressed = -1;
    }
    dirty(live);
    if (element.image && !element.image->submitted) wake(gui.owner);
    lua_pushboolean(lua, true); return 1;
}

int element_state(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, element_type);
    auto& surface = live_surface(gui, identity); layout(surface);
    const auto& element = surface.elements[static_cast<std::size_t>(identity.element)];
    const auto text = element.text; const auto box = element.box; const auto value = element.value; const auto scroll = element.scroll;
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 6); string_field(lua, "text", text);
        lua_pushnumber(lua, value); lua_setfield(lua, -2, "value");
        lua_pushnumber(lua, box.x); lua_setfield(lua, -2, "x");
        lua_pushnumber(lua, box.y); lua_setfield(lua, -2, "y");
        lua_pushnumber(lua, box.w); lua_setfield(lua, -2, "width");
        lua_pushnumber(lua, box.h); lua_setfield(lua, -2, "height");
        lua_pushnumber(lua, scroll); lua_setfield(lua, -2, "scroll");
        return 1;
    }, 1);
}

int focus_element(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, element_type);
    auto& surface = live_surface(gui, identity);
    if (!focusable(surface.elements[static_cast<std::size_t>(identity.element)])) throw std::runtime_error("element is not focusable");
    focus(surface, identity.element);
    lua_pushboolean(lua, true); return 1;
}

const hyprland_focus_grab_v1_listener grab_listener{
    .cleared = [](void* data, hyprland_focus_grab_v1*) { static_cast<Surface*>(data)->grab_pending = true; }
};

template<int Role>
int create_surface(lua_State* lua) {
    auto& gui = gui_state(lua);
    luaL_checktype(lua, 1, LUA_TTABLE);
    auto surface = std::make_unique<Surface>(); surface->gui = &gui; surface->id = ++gui.next_surface;
    struct Cleanup {
        Surface* surface;
        ~Cleanup() { if (surface) destroy_surface(*surface); }
    } cleanup{surface.get()};
    std::string title = "Hyprworks", app_id = "hyprworks", output;
    int width = Role == 1 ? 0 : 640, height = Role == 1 ? 32 : 480, x = 0, y = 0;
    int edge = 0, layer = 2, keyboard = 0, exclusive_zone = 0, margin = 0;
    bool exclusive = false, grab = false;
    lua_pushvalue(lua, 1);
    if (protected_call(lua, [&](lua_State* lua) {
        width = static_cast<int>(integer_field(lua, 2, "width", width, Role == 1 ? 0 : 1));
        height = static_cast<int>(integer_field(lua, 2, "height", height, Role == 1 ? 0 : 1));
        text_field(lua, 2, "title", title, 256); text_field(lua, 2, "app_id", app_id, 256);
        text_field(lua, 2, "output", output, 256);
        callback_field(lua, 2, "on_configure", surface->on_configure);
        callback_field(lua, 2, "on_close", surface->on_close);
        callback_field(lua, 2, "on_grab_cleared", surface->on_grab_cleared);
        callback_field(lua, 2, "on_input", surface->on_input);
        if constexpr (Role == 1) {
            static const char* edges[] = {"top", "bottom", "left", "right", "all", nullptr};
            static const char* layers[] = {"background", "bottom", "top", "overlay", nullptr};
            static const char* keyboards[] = {"none", "exclusive", "on_demand", nullptr};
            edge = enum_field(lua, 2, "edge", "top", edges);
            layer = enum_field(lua, 2, "layer", "top", layers);
            keyboard = enum_field(lua, 2, "keyboard", "none", keyboards);
            exclusive = boolean_field(lua, 2, "exclusive", false);
            exclusive_zone = static_cast<int>(integer_field(lua, 2, "exclusive_zone", 0, -1));
            margin = static_cast<int>(integer_field(lua, 2, "margin", 0, 0, 4096));
            if (edge == 4) {
                width = static_cast<int>(integer_field(lua, 2, "width", 0, 0));
                height = static_cast<int>(integer_field(lua, 2, "height", 0, 0));
                if (exclusive || exclusive_zone > 0) luaL_error(lua, "an all-edge layer cannot reserve space");
            } else if (edge >= 2) {
                width = static_cast<int>(integer_field(lua, 2, "width", 32, 1));
                height = static_cast<int>(integer_field(lua, 2, "height", 0, 0));
            } else height = static_cast<int>(integer_field(lua, 2, "height", 32, 1));
        }
        if constexpr (Role == 2) {
            field(lua, 2, "parent"); surface->parent = handle(lua, -1, surface_type).surface; lua_pop(lua, 1);
            x = static_cast<int>(integer_field(lua, 2, "x", 0, -16384));
            y = static_cast<int>(integer_field(lua, 2, "y", 0, -16384));
            grab = boolean_field(lua, 2, "grab", false);
        }
        return 0;
    }, 0, 0, 1) < 0) return -1;
    if (gui.owner.closing) throw std::runtime_error("runtime is stopping");
    auto slot = std::ranges::find_if(gui.surfaces, [](const auto& item) { return !item; });
    if (slot == gui.surfaces.end()) throw std::runtime_error("surface limit reached (32); closed surfaces retire at the next loop turn");
    Surface* parent = nullptr;
    if constexpr (Role == 1) if (!gui.layers) throw std::runtime_error("layer-shell v4 is unavailable");
    if constexpr (Role == 2) {
        parent = find_surface(gui, surface->parent);
        if (!parent || !parent->configured) throw std::runtime_error("popup parent must be configured");
        for (const auto& item : gui.surfaces)
            if (item && item->parent == parent->id) throw std::runtime_error("parent already has an active or retiring popup");
        if (grab && (!gui.event_seat || !gui.event_serial || gui.event_surface != parent->id))
            throw std::runtime_error("popup grab requires a parent input callback");
    }
    wl_output* native_output = nullptr;
    if (!output.empty()) {
        for (const auto& item : gui.outputs) if (item->name == output) native_output = item->native;
        if (!native_output) throw std::runtime_error("output not found: " + output);
    }
    surface->pending_width = surface->width = width ? width : 640;
    surface->pending_height = surface->height = height ? height : 480;
    surface->native = wl_compositor_create_surface(gui.compositor);
    if (!surface->native) throw std::runtime_error("cannot create Wayland surface");
    wl_surface_add_listener(surface->native, &surface_listener, surface.get());
    if (gui.viewporter) {
        surface->viewport = wp_viewporter_get_viewport(gui.viewporter, surface->native);
        if (!surface->viewport) throw std::runtime_error("cannot create viewport");
    }
    if (gui.fractions && surface->viewport) {
        surface->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(gui.fractions, surface->native);
        if (!surface->fractional) throw std::runtime_error("cannot create fractional scale");
        wp_fractional_scale_v1_add_listener(surface->fractional, &scale_listener, surface.get());
    }
    if constexpr (Role == 1) {
        surface->layer = zwlr_layer_shell_v1_get_layer_surface(gui.layers, surface->native, native_output, static_cast<std::uint32_t>(layer), app_id.c_str());
        if (!surface->layer) throw std::runtime_error("cannot create layer surface");
        zwlr_layer_surface_v1_add_listener(surface->layer, &layer_listener, surface.get());
        const std::uint32_t anchors[] = {1 | 4 | 8, 2 | 4 | 8, 4 | 1 | 2, 8 | 1 | 2, 1 | 2 | 4 | 8};
        zwlr_layer_surface_v1_set_anchor(surface->layer, anchors[edge]);
        zwlr_layer_surface_v1_set_size(surface->layer, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer, static_cast<std::uint32_t>(keyboard));
        zwlr_layer_surface_v1_set_margin(surface->layer, margin, margin, margin, margin);
        zwlr_layer_surface_v1_set_exclusive_zone(surface->layer, exclusive ? (edge < 2 ? height : width) + margin : exclusive_zone);
    } else {
        surface->xdg = xdg_wm_base_get_xdg_surface(gui.shell, surface->native);
        if (!surface->xdg) throw std::runtime_error("cannot create XDG surface");
        xdg_surface_add_listener(surface->xdg, &xdg_listener, surface.get());
        if constexpr (Role == 0) {
            surface->toplevel = xdg_surface_get_toplevel(surface->xdg);
            if (!surface->toplevel) throw std::runtime_error("cannot create toplevel");
            xdg_toplevel_add_listener(surface->toplevel, &toplevel_listener, surface.get());
            xdg_toplevel_set_title(surface->toplevel, title.c_str()); xdg_toplevel_set_app_id(surface->toplevel, app_id.c_str());
        } else {
            auto* position = xdg_wm_base_create_positioner(gui.shell);
            if (!position) throw std::runtime_error("cannot create popup positioner");
            xdg_positioner_set_size(position, width, height);
            xdg_positioner_set_anchor_rect(position, std::clamp(x, 0, parent->width - 1), std::clamp(y, 0, parent->height - 1), 1, 1);
            xdg_positioner_set_anchor(position, XDG_POSITIONER_ANCHOR_TOP_LEFT);
            xdg_positioner_set_gravity(position, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
            xdg_positioner_set_constraint_adjustment(position, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
            surface->popup = xdg_surface_get_popup(surface->xdg, parent->xdg, position);
            xdg_positioner_destroy(position);
            if (!surface->popup) throw std::runtime_error("cannot create popup");
            xdg_popup_add_listener(surface->popup, &popup_listener, surface.get());
            if (parent->layer) zwlr_layer_surface_v1_get_popup(parent->layer, surface->popup);
            if (grab) xdg_popup_grab(surface->popup, gui.event_seat->native, gui.event_serial);
        }
    }
    const auto id = surface->id;
    wl_surface_commit(surface->native);
    *slot = std::move(surface); cleanup.surface = nullptr;
    if (protected_call(lua, [&](lua_State* lua) { push_handle(lua, {id, 0, -1}, surface_type); return 1; }, 1) < 0) {
        close_surface(**slot); return -1;
    }
    return 1;
}

int surface_close(lua_State* lua) {
    const auto identity = handle(lua, 1, surface_type);
    auto& state = invocation(lua);
    if (!state.closing && state.gui)
        if (auto* surface = find_surface(*state.gui, identity.surface)) close_surface(*surface);
    lua_pushboolean(lua, true); return 1;
}

int surface_state(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, surface_type);
    const auto* surface = find_surface(gui, identity.surface);
    const int width = surface ? surface->width : 0, height = surface ? surface->height : 0;
    const double scale = surface ? surface->scale : 1; const bool configured = surface && surface->configured;
    lua_createtable(lua, 0, 5);
    lua_pushboolean(lua, !surface); lua_setfield(lua, -2, "closed");
    lua_pushboolean(lua, configured); lua_setfield(lua, -2, "configured");
    lua_pushinteger(lua, width); lua_setfield(lua, -2, "width");
    lua_pushinteger(lua, height); lua_setfield(lua, -2, "height");
    lua_pushnumber(lua, scale); lua_setfield(lua, -2, "scale"); return 1;
}

int grab_focus(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, surface_type);
    auto& surface = live_surface(gui, identity);
    if (!gui.focus_grabs || !surface.configured) throw std::runtime_error("focus grab requires a configured surface and compositor support");
    if (!surface.grab) {
        surface.grab = hyprland_focus_grab_manager_v1_create_grab(gui.focus_grabs);
        if (!surface.grab) throw std::runtime_error("cannot create focus grab");
        hyprland_focus_grab_v1_add_listener(surface.grab, &grab_listener, &surface);
    }
    hyprland_focus_grab_v1_add_surface(surface.grab, surface.native);
    hyprland_focus_grab_v1_commit(surface.grab);
    lua_pushboolean(lua, true); return 1;
}

int release_focus(lua_State* lua) {
    auto& gui = gui_state(lua); auto& surface = live_surface(gui, handle(lua, 1, surface_type));
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    lua_pushboolean(lua, true); return 1;
}

int input_region(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, surface_type);
    const bool reset = lua_isnoneornil(lua, 2);
    if (!reset) luaL_checktype(lua, 2, LUA_TTABLE);
    std::vector<std::array<int, 4>> rectangles;
    if (!reset) {
        lua_pushvalue(lua, 2);
        if (protected_call(lua, [&](lua_State* lua) {
            const auto count = lua_rawlen(lua, 2);
            if (count > 256) luaL_error(lua, "input region exceeds 256 rectangles");
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(lua, 2, static_cast<lua_Integer>(i)); luaL_checktype(lua, -1, LUA_TTABLE);
                const int x = static_cast<int>(integer_field(lua, -1, "x", 0, -16384));
                const int y = static_cast<int>(integer_field(lua, -1, "y", 0, -16384));
                const int width = static_cast<int>(integer_field(lua, -1, "width", 0, 0));
                const int height = static_cast<int>(integer_field(lua, -1, "height", 0, 0));
                rectangles.push_back({x, y, width, height}); lua_pop(lua, 1);
            }
            return 0;
        }, 0, 0, 1) < 0) return -1;
    }
    auto& surface = live_surface(gui, identity);
    auto* region = reset ? nullptr : wl_compositor_create_region(gui.compositor);
    if (!reset && !region) throw std::runtime_error("cannot allocate input region");
    for (const auto& box : rectangles) wl_region_add(region, box[0], box[1], box[2], box[3]);
    wl_surface_set_input_region(surface.native, region);
    if (region) wl_region_destroy(region);
    surface.dirty = true; lua_pushboolean(lua, true); return 1;
}

int outputs(lua_State* lua) {
    auto& gui = gui_state(lua);
    struct Record { std::string name, description; int scale; };
    std::vector<Record> records;
    for (const auto& output : gui.outputs) records.push_back({output->name, output->description, output->scale});
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, static_cast<int>(records.size()), 0);
        for (std::size_t i = 0; i < records.size(); ++i) {
            lua_createtable(lua, 0, 3); string_field(lua, "name", records[i].name); string_field(lua, "description", records[i].description);
            lua_pushinteger(lua, records[i].scale); lua_setfield(lua, -2, "scale"); lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }, 1);
}

int on_outputs(lua_State* lua) {
    auto& gui = gui_state(lua);
    gui.outputs_pending = replace_callback(lua, 1, gui.on_outputs, gui.owner.closing);
    lua_pushboolean(lua, true); return 1;
}

int capabilities(lua_State* lua) {
    auto& gui = gui_state(lua);
    const bool layer = gui.layers, fraction = gui.fractions && gui.viewporter, ime = gui.text_input, clipboard = gui.data, grab = gui.focus_grabs;
    lua_createtable(lua, 0, 5);
    lua_pushboolean(lua, layer); lua_setfield(lua, -2, "layer_shell");
    lua_pushboolean(lua, fraction); lua_setfield(lua, -2, "fractional_scale");
    lua_pushboolean(lua, ime); lua_setfield(lua, -2, "text_input");
    lua_pushboolean(lua, clipboard); lua_setfield(lua, -2, "clipboard");
    lua_pushboolean(lua, grab); lua_setfield(lua, -2, "focus_grab"); return 1;
}

int notify(Gui& gui, std::uint64_t id, int element_index, int reference, const char* kind = nullptr, const std::string* error = nullptr) {
    if (reference < 0 || gui.owner.quit_requested.load(std::memory_order_relaxed)) return 0;
    auto* surface = id ? find_surface(gui, id) : nullptr;
    if (id && !surface) return 0;
    const auto revision = surface ? surface->revision : 0;
    std::string text, message = error ? *error : "";
    double value = 0;
    if (surface && element_index >= 0) {
        const auto& element = surface->elements[static_cast<std::size_t>(element_index)];
        text = element.text; value = element.value;
    }
    lua_pushcfunction(gui.owner.lua, traceback);
    const int result = protected_call(gui.owner.lua, [&](lua_State* lua) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
        if (id) push_handle(lua, {id, revision, element_index}, element_index < 0 ? surface_type : element_type);
        if (element_index >= 0) {
            lua_createtable(lua, 0, 4);
            string_field(lua, "text", text); lua_pushnumber(lua, value); lua_setfield(lua, -2, "value");
            if (kind) { lua_pushstring(lua, kind); lua_setfield(lua, -2, "kind"); }
            if (!message.empty()) string_field(lua, "error", message);
        }
        if (id) {
            auto* live = find_surface(gui, id);
            if (!live || (element_index >= 0 && (live->revision != revision ||
                live->elements[static_cast<std::size_t>(element_index)].callback != reference))) return 0;
        }
        lua_call(lua, id ? (element_index >= 0 ? 2 : 1) : 0, 0); return 0;
    }, 0, 1);
    if (result >= 0) lua_settop(gui.owner.lua, 0);
    return result;
}

void update_ime(Seat& seat) {
    if (!seat.ime) return;
    auto* surface = find_surface(*seat.gui, seat.ime_surface);
    Element* element = surface && surface->focused >= 0 ? &surface->elements[static_cast<std::size_t>(surface->focused)] : nullptr;
    if (!element || element->kind != Kind::input || !element->enabled || element->password) {
        if (seat.ime_enabled) {
            zwp_text_input_v3_disable(seat.ime); zwp_text_input_v3_commit(seat.ime); ++seat.ime_serial;
        }
        seat.ime_enabled = false; seat.ime_element = -1; return;
    }
    layout(*surface);
    const auto cursor = input_cursor(*element, true);
    const std::array<int, 4> rectangle{
        static_cast<int>(element->box.x + element->padding - element->scroll) + cursor.x / PANGO_SCALE,
        static_cast<int>(element->box.y + element->padding) + cursor.y / PANGO_SCALE,
        1, std::max(1, cursor.height / PANGO_SCALE)};
    const int selection_begin = std::min(element->cursor, element->anchor), selection_end = std::max(element->cursor, element->anchor);
    const bool surrounding = selection_end - selection_begin <= 3999;
    const bool target_changed = !seat.ime_enabled || seat.ime_revision != surface->revision ||
        seat.ime_element != surface->focused;
    if (!target_changed && seat.ime_blocked) return;
    const bool reset = target_changed || seat.ime_surrounding != surrounding;
    if (reset) {
        if (seat.ime_enabled) zwp_text_input_v3_disable(seat.ime);
        zwp_text_input_v3_enable(seat.ime);
        zwp_text_input_v3_set_content_type(seat.ime, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        seat.ime_enabled = true; seat.ime_blocked = false;
    } else if (seat.ime_edit == element->edit_revision && seat.ime_cursor == rectangle) return;
    // The protocol caps surrounding text at 4000 bytes, including its terminator.
    if (surrounding) {
        int begin = std::max(0, selection_begin - (3999 - (selection_end - selection_begin)) / 2);
        while (begin < selection_begin && (static_cast<unsigned char>(element->text[static_cast<std::size_t>(begin)]) & 0xc0) == 0x80) ++begin;
        int end = std::min(static_cast<int>(element->text.size()), begin + 3999);
        while (end > begin && end < static_cast<int>(element->text.size()) && (static_cast<unsigned char>(element->text[static_cast<std::size_t>(end)]) & 0xc0) == 0x80) --end;
        const auto text = element->text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
        zwp_text_input_v3_set_surrounding_text(seat.ime, text.c_str(), element->cursor - begin, element->anchor - begin);
    }
    const auto cause = reset || (seat.ime_edit != element->edit_revision && seat.ime_applied_edit != element->edit_revision) ?
        ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER : ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;
    zwp_text_input_v3_set_text_change_cause(seat.ime, cause);
    zwp_text_input_v3_set_cursor_rectangle(seat.ime, rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
    zwp_text_input_v3_commit(seat.ime); ++seat.ime_serial;
    seat.ime_revision = surface->revision; seat.ime_element = surface->focused; seat.ime_edit = element->edit_revision;
    seat.ime_cursor = rectangle;
    seat.ime_surrounding = surrounding;
}

bool replace_text(Surface& surface, Element& element, int begin, int end, const std::string& inserted) {
    if (!valid_text(inserted) || begin < 0 || end < begin || end > static_cast<int>(element.text.size())) return false;
    const auto boundary = [&](int index) { return index == static_cast<int>(element.text.size()) || (static_cast<unsigned char>(element.text[static_cast<std::size_t>(index)]) & 0xc0) != 0x80; };
    if (!boundary(begin) || !boundary(end)) return false;
    const auto size = element.text.size() - static_cast<std::size_t>(end - begin) + inserted.size();
    if (size > max_text || surface.content_bytes - element.text.size() + size > max_content) return false;
    const auto previous = element.text.size();
    element.text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin), inserted);
    surface.content_bytes = surface.content_bytes - previous + size;
    element.cursor = element.anchor = begin + static_cast<int>(inserted.size());
    element.preedit.clear(); ++element.edit_revision; dirty(surface); return true;
}

int adjacent_cursor(Element& element, int position, int direction) {
    int count = 0;
    std::vector<PangoLogAttr> password_attrs;
    const PangoLogAttr* attrs;
    if (element.password) {
        count = static_cast<int>(g_utf8_strlen(element.text.c_str(), -1)) + 1;
        password_attrs.resize(static_cast<std::size_t>(count));
        pango_get_log_attrs(element.text.c_str(), static_cast<int>(element.text.size()), -1,
            pango_context_get_language(pango_layout_get_context(element.layout)), password_attrs.data(), count);
        attrs = password_attrs.data();
    } else attrs = pango_layout_get_log_attrs_readonly(element.layout, &count);
    int character = static_cast<int>(g_utf8_pointer_to_offset(element.text.c_str(), element.text.c_str() + position));
    for (character += direction; character >= 0 && character < count; character += direction) {
        if (attrs[character].is_cursor_position)
            return static_cast<int>(g_utf8_offset_to_pointer(element.text.c_str(), character) - element.text.c_str());
    }
    return direction < 0 ? 0 : static_cast<int>(element.text.size());
}

void copy_selection(Seat& seat, const std::string& text) {
    if (!seat.gui->data || !seat.data_device || !seat.serial) throw std::runtime_error("clipboard write requires seat input");
    if (text.size() > 1024 * 1024 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
        throw std::runtime_error("clipboard text must be UTF-8 and at most 1 MiB");
    seat.clipboard = text;
    if (seat.source) wl_data_source_destroy(seat.source);
    seat.source = wl_data_device_manager_create_data_source(seat.gui->data);
    if (!seat.source) throw std::runtime_error("cannot create clipboard source");
    wl_data_source_add_listener(seat.source, &source_listener, &seat);
    wl_data_source_offer(seat.source, "text/plain;charset=utf-8"); wl_data_source_offer(seat.source, "text/plain");
    wl_data_device_set_selection(seat.data_device, seat.source, seat.serial);
}

void paste(Seat& seat, Surface* surface, int callback = LUA_NOREF) {
    auto& gui = *seat.gui;
    if (!seat.selection || !seat.selection->mime) throw std::runtime_error("clipboard has no text offer");
    if (gui.transfers.size() >= 16) throw std::runtime_error("clipboard transfer limit reached");
    Transfer transfer;
    transfer.deadline = Clock::now() + std::chrono::seconds(5);
    if (surface) {
        transfer.surface = surface->id; transfer.revision = surface->revision; transfer.element = surface->focused;
        transfer.edit_revision = surface->elements[static_cast<std::size_t>(surface->focused)].edit_revision;
    }
    gui.transfers.push_back(std::move(transfer));
    int pipe[2];
    if (pipe2(pipe, O_CLOEXEC) < 0) { gui.transfers.pop_back(); throw std::runtime_error("cannot create clipboard pipe"); }
    if (fcntl(pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipe[0]); close(pipe[1]); gui.transfers.pop_back();
        throw std::runtime_error("cannot make clipboard reader nonblocking");
    }
    gui.transfers.back().fd = pipe[0]; gui.transfers.back().callback = callback;
    wl_data_offer_receive(seat.selection->native, seat.selection->mime, pipe[1]); close(pipe[1]);
}

int clipboard_read(lua_State* lua) {
    auto& gui = gui_state(lua);
    luaL_checktype(lua, 1, LUA_TFUNCTION);
    Seat* seat = gui.event_seat;
    if (!seat && !gui.seats.empty()) seat = gui.seats.front().get();
    if (!seat) throw std::runtime_error("clipboard requires a seat");
    lua_pushvalue(lua, 1); int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
    try { paste(*seat, nullptr, reference); }
    catch (...) { unref(lua, reference); throw; }
    lua_pushboolean(lua, true); return 1;
}

int clipboard_write(lua_State* lua) {
    auto& gui = gui_state(lua);
    std::size_t length; const char* text = luaL_checklstring(lua, 1, &length);
    if (length > 1024 * 1024) luaL_error(lua, "clipboard text exceeds 1 MiB");
    if (!gui.event_seat) throw std::runtime_error("clipboard write requires an input callback");
    copy_selection(*gui.event_seat, std::string(text, length)); lua_pushboolean(lua, true); return 1;
}

ssize_t write_pipe(int fd, const char* data, std::size_t size) {
    sigset_t block, previous, pending;
    sigemptyset(&block); sigaddset(&block, SIGPIPE);
    const int error = pthread_sigmask(SIG_BLOCK, &block, &previous);
    if (error) { errno = error; return -1; }
    sigpending(&pending); const bool existed = sigismember(&pending, SIGPIPE);
    const auto result = write(fd, data, size); const int saved = errno;
    if (result < 0 && saved == EPIPE && !existed) {
        timespec timeout{};
        while (sigtimedwait(&block, nullptr, &timeout) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr); errno = saved; return result;
}

void transfer_io(Transfer& transfer, short events) {
    if (transfer.fd < 0) return;
    if (Clock::now() >= transfer.deadline) transfer.error = "clipboard transfer timed out";
    else if (events & POLLNVAL) transfer.error = "invalid clipboard descriptor";
    else if (events) {
        std::size_t processed = 0;
        while (processed < 65536) {
            char bytes[8192];
            const auto count = transfer.writing ?
                write_pipe(transfer.fd, transfer.data.data() + transfer.offset, std::min<std::size_t>(8192, transfer.data.size() - transfer.offset)) :
                read(transfer.fd, bytes, sizeof(bytes));
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) transfer.error = std::strerror(errno);
                break;
            }
            if (!count) {
                close(std::exchange(transfer.fd, -1)); break;
            }
            processed += static_cast<std::size_t>(count);
            if (transfer.writing) {
                transfer.offset += static_cast<std::size_t>(count);
                if (transfer.offset == transfer.data.size()) { close(std::exchange(transfer.fd, -1)); break; }
            } else {
                if (transfer.data.size() + static_cast<std::size_t>(count) > 1024 * 1024) { transfer.error = "clipboard exceeds 1 MiB"; break; }
                transfer.data.append(bytes, static_cast<std::size_t>(count));
            }
        }
    }
    if (!transfer.error.empty() && transfer.fd >= 0) close(std::exchange(transfer.fd, -1));
}

void position_input_cursor(Surface& surface, Element& element, double x, double y, bool select) {
    if (!element.preedit.empty()) { element.preedit.clear(); dirty(surface); layout(surface); }
    int index = 0, trailing = 0;
    const auto units = [](double value) {
        return static_cast<int>(std::clamp(value * PANGO_SCALE,
            static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())));
    };
    pango_layout_xy_to_index(element.layout,
        units(x - element.box.x - element.padding + element.scroll),
        units(y - element.box.y - element.padding), &index, &trailing);
    index = std::clamp(index, 0, layout_index(element, static_cast<int>(element.text.size())));
    const auto* point = element.password ? g_utf8_offset_to_pointer(element.text.c_str(), index) : element.text.c_str() + index;
    while (trailing-- > 0 && *point) point = g_utf8_next_char(point);
    element.cursor = static_cast<int>(point - element.text.c_str());
    if (!select) element.anchor = element.cursor;
    ++element.edit_revision; surface.dirty = true;
}

int process_input(Gui& gui, Input& event) {
    auto* surface = find_surface(gui, event.surface);
    if (!surface) return -1;
    auto& s = *surface;
    layout(s);
    const bool owns_press = s.press_seat == event.seat && s.press_touch == event.touch;
    if (event.type == Input::Type::blur) {
        if (s.press_seat == event.seat) s.pressed = -1;
        if (s.focused >= 0) {
            auto& element = s.elements[static_cast<std::size_t>(s.focused)];
            if (element.preedit.empty() || element.preedit_seat == event.seat) {
                element.preedit.clear(); ++element.edit_revision; dirty(s);
            }
        }
        return -1;
    }
    if (event.type == Input::Type::leave) {
        if (owns_press) s.pressed = -1;
        return -1;
    }
    if (event.type == Input::Type::focus) { s.dirty = true; return -1; }
    if (event.type == Input::Type::axis) {
        auto* seat = find_seat(gui, event.seat);
        if (!seat) return -1;
        for (int i = static_cast<int>(s.elements.size()) - 1; i >= 0; --i) {
            auto& element = s.elements[static_cast<std::size_t>(i)];
            if (element.kind == Kind::scroll && element.clip.contains(event.x, event.y)) {
                element.scroll = std::max(0.0, element.scroll + event.delta); dirty(s); break;
            }
        }
        return -1;
    }
    int target = -1;
    for (int i = static_cast<int>(s.elements.size()) - 1; i >= 0; --i)
        if (focusable(s.elements[static_cast<std::size_t>(i)]) && s.elements[static_cast<std::size_t>(i)].clip.contains(event.x, event.y)) { target = i; break; }
    if (event.type == Input::Type::button && event.key == BTN_LEFT) {
        if (event.pressed) {
            s.pressed = target; s.press_seat = event.seat; s.press_touch = event.touch;
            focus(s, target); s.press_serial = event.serial; s.dirty = true;
            if (target >= 0) {
                auto& element = s.elements[static_cast<std::size_t>(target)];
                if (element.kind == Kind::input) position_input_cursor(s, element, event.x, event.y, false);
            }
        } else {
            if (!owns_press) return -1;
            const int pressed = std::exchange(s.pressed, -1);
            if (pressed < 0 || pressed != target) return -1;
            auto& element = s.elements[static_cast<std::size_t>(target)];
            gui.event_serial = s.press_serial;
            if (element.kind == Kind::button) return target;
            if (element.kind == Kind::toggle) { element.value = element.value == 0 ? 1 : 0; s.dirty = true; return target; }
        }
    }
    if (((event.type == Input::Type::motion && owns_press) ||
        (event.type == Input::Type::button && event.key == BTN_LEFT && event.pressed)) && s.pressed >= 0) {
        auto& element = s.elements[static_cast<std::size_t>(s.pressed)];
        if (element.kind == Kind::slider) {
            const double value = std::clamp((event.x - element.box.x - 4) / std::max(1.0, element.box.w - 8), 0.0, 1.0);
            if (value != element.value) { element.value = value; s.dirty = true; return s.pressed; }
        } else if (element.kind == Kind::input && event.type == Input::Type::motion) {
            position_input_cursor(s, element, event.x, event.y, true);
        }
    }
    if (event.type == Input::Type::key && event.pressed && (event.key == XKB_KEY_Tab || event.key == XKB_KEY_ISO_Left_Tab)) {
        const int count = static_cast<int>(s.elements.size()), direction = event.shift || event.key == XKB_KEY_ISO_Left_Tab ? -1 : 1;
        const int start = s.focused >= 0 ? s.focused : direction > 0 ? -1 : 0;
        for (int i = 1; i <= count; ++i) {
            const int next = (start + direction * i + count * 2) % count;
            if (focusable(s.elements[static_cast<std::size_t>(next)])) { focus(s, next); break; }
        }
        return -1;
    }
    if (s.focused < 0) return -1;
    auto& element = s.elements[static_cast<std::size_t>(s.focused)];
    if (!focusable(element)) return -1;
    if (event.type == Input::Type::ime_leave) {
        if (event.revision == s.revision && event.element == s.focused &&
            !element.preedit.empty() && element.preedit_seat == event.seat) {
            element.preedit.clear(); dirty(s);
        }
        return -1;
    }
    if (event.type == Input::Type::ime) {
        if (event.revision != s.revision || event.element != s.focused || element.kind != Kind::input || element.password) return -1;
        auto* seat = find_seat(gui, event.seat);
        if (seat) seat->ime_blocked = event.serial != seat->ime_serial;
        bool changed = false;
        const int selection_begin = std::min(element.cursor, element.anchor), selection_end = std::max(element.cursor, element.anchor);
        if (event.before > static_cast<std::uint32_t>(selection_begin) || event.after > element.text.size() - static_cast<std::size_t>(selection_end) ||
            (event.commit_set && !valid_text(event.text)) || (event.preedit_set && !valid_text(event.preedit))) return -1;
        if (event.preedit_set) {
            const auto boundary = [&](int index) {
                return index == -1 || (index >= 0 && index <= static_cast<int>(event.preedit.size()) &&
                    (index == static_cast<int>(event.preedit.size()) || (static_cast<unsigned char>(event.preedit[static_cast<std::size_t>(index)]) & 0xc0) != 0x80));
            };
            if (!boundary(event.preedit_cursor) || !boundary(event.preedit_end)) return -1;
        }
        const bool replace_selection = event.commit_set ||
            (event.preedit_set && !event.preedit.empty() && selection_begin != selection_end);
        if (replace_selection || event.before || event.after) {
            const bool backwards = element.cursor < element.anchor;
            const auto insertion = event.commit_set ? event.text : replace_selection ? std::string{} :
                element.text.substr(static_cast<std::size_t>(selection_begin), static_cast<std::size_t>(selection_end - selection_begin));
            const int start = selection_begin - static_cast<int>(event.before);
            changed = replace_text(s, element, start, selection_end + static_cast<int>(event.after), insertion);
            if (!changed) return -1;
            if (!replace_selection) {
                element.cursor = backwards ? start : start + static_cast<int>(insertion.size());
                element.anchor = backwards ? start + static_cast<int>(insertion.size()) : start;
            }
        }
        element.preedit = event.preedit_set ? event.preedit : "";
        element.preedit_seat = event.seat;
        element.preedit_cursor = event.preedit_cursor; element.preedit_end = event.preedit_end; dirty(s);
        if (changed && seat) seat->ime_applied_edit = element.edit_revision;
        return changed ? s.focused : -1;
    }
    if (event.type != Input::Type::key || !event.pressed) return -1;
    if (element.kind == Kind::button && (event.key == XKB_KEY_space || event.key == XKB_KEY_Return)) return s.focused;
    if (element.kind == Kind::toggle && event.key == XKB_KEY_space) {
        element.value = element.value == 0 ? 1 : 0; s.dirty = true; return s.focused;
    }
    if (element.kind == Kind::slider && (event.key == XKB_KEY_Left || event.key == XKB_KEY_Right)) {
        element.value = std::clamp(element.value + (event.key == XKB_KEY_Left ? -0.01 : 0.01), 0.0, 1.0);
        s.dirty = true; return s.focused;
    }
    if (element.kind != Kind::input) return -1;
    const int begin = std::min(element.cursor, element.anchor), end = std::max(element.cursor, element.anchor);
    auto* seat = find_seat(gui, event.seat);
    if (event.ctrl && (event.key == XKB_KEY_a || event.key == XKB_KEY_A)) {
        element.preedit.clear();
        element.anchor = 0; element.cursor = static_cast<int>(element.text.size()); ++element.edit_revision; dirty(s); return -1;
    }
    if (event.ctrl && seat) {
        if (event.key == XKB_KEY_c || event.key == XKB_KEY_C || event.key == XKB_KEY_x || event.key == XKB_KEY_X) {
            if (element.password) return -1;
            if (begin != end) {
                copy_selection(*seat, element.text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)));
                if (event.key == XKB_KEY_x || event.key == XKB_KEY_X)
                    return replace_text(s, element, begin, end, "") ? s.focused : -1;
            }
            return -1;
        }
        if (event.key == XKB_KEY_v || event.key == XKB_KEY_V) {
            if (seat->selection && seat->selection->mime) paste(*seat, &s);
            return -1;
        }
    }
    if (!element.preedit.empty()) return -1;
    if (event.key == XKB_KEY_BackSpace || event.key == XKB_KEY_Delete) {
        const int start = begin != end ? begin : event.key == XKB_KEY_BackSpace ? adjacent_cursor(element, begin, -1) : begin;
        const int finish = begin != end ? end : event.key == XKB_KEY_Delete ? adjacent_cursor(element, end, 1) : end;
        return start != finish && replace_text(s, element, start, finish, "") ? s.focused : -1;
    }
    if (event.key == XKB_KEY_Left || event.key == XKB_KEY_Right || event.key == XKB_KEY_Home || event.key == XKB_KEY_End) {
        if (event.key == XKB_KEY_Home) element.cursor = 0;
        else if (event.key == XKB_KEY_End) element.cursor = static_cast<int>(element.text.size());
        else if (element.password) element.cursor = adjacent_cursor(element, element.cursor, event.key == XKB_KEY_Left ? -1 : 1);
        else {
            int position = 0, trailing = 0;
            pango_layout_move_cursor_visually(element.layout, true, element.cursor, 0, event.key == XKB_KEY_Left ? -1 : 1, &position, &trailing);
            position = std::clamp(position, 0, static_cast<int>(element.text.size()));
            const char* point = element.text.c_str() + position;
            while (trailing-- > 0 && *point) point = g_utf8_next_char(point);
            element.cursor = static_cast<int>(point - element.text.c_str());
        }
        if (!event.shift) element.anchor = element.cursor;
        ++element.edit_revision; s.dirty = true; return -1;
    }
    return !event.text.empty() && replace_text(s, element, begin, end, event.text) ? s.focused : -1;
}

template<int (*Function)(lua_State*)>
int gui_call(lua_State* lua) {
    char message[1024];
    int result = 0; bool failed = false;
    try { result = Function(lua); }
    catch (const std::exception& error) { std::snprintf(message, sizeof(message), "%s", error.what()); failed = true; }
    if (failed) { lua_pushnil(lua); lua_pushstring(lua, message); return 2; }
    return result < 0 ? lua_error(lua) : result;
}

}

Buffer::~Buffer() {
    if (native) wl_buffer_destroy(native);
    if (cairo) cairo_surface_destroy(cairo);
    if (mapping && mapping != MAP_FAILED) munmap(mapping, bytes);
    gui->buffer_bytes -= bytes;
}

Gui::Gui(InvocationState& state) : owner(state) {}

void start_gui(InvocationState& state) {
    state.gui = std::make_unique<Gui>(state);
    auto& gui = *state.gui;
    gui.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!gui.xkb) throw std::runtime_error("cannot initialize xkbcommon");
    const char* locale = std::getenv("LC_ALL");
    if (!locale || !*locale) locale = std::getenv("LC_CTYPE");
    if (!locale || !*locale) locale = std::getenv("LANG");
    gui.compose = xkb_compose_table_new_from_locale(gui.xkb, locale && *locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    gui.fonts = pango_cairo_font_map_new();
    if (!gui.fonts) throw std::runtime_error("cannot initialize Pango font map");
    gui.text_context = pango_font_map_create_context(gui.fonts);
    if (!gui.text_context) throw std::runtime_error("cannot initialize Pango");
    gui.display = wl_display_connect(nullptr);
    if (!gui.display) throw std::runtime_error("cannot connect to Wayland");
    gui.registry = wl_display_get_registry(gui.display);
    if (!gui.registry) throw std::runtime_error("cannot create Wayland registry");
    wl_registry_add_listener(gui.registry, &registry_listener, &gui);
    synchronize(gui); synchronize(gui);
    if (!gui.compositor || !gui.shm || !gui.shell) throw std::runtime_error("compositor requires wl_compositor v4, wl_shm and xdg-shell");
    gui.images.start(state.wake_fd);
}

void close_gui(Gui& gui) noexcept {
    if (gui.stopping) return;
    gui.stopping = true;
    auto& state = gui.owner;
    if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
    gui.images.stop();
    for (auto& surface : gui.surfaces) if (surface) destroy_surface(*surface);
    for (auto& transfer : gui.transfers) {
        if (transfer.fd >= 0) close(transfer.fd);
        unref(state.lua, transfer.callback);
    }
    gui.transfers.clear();
    for (auto& seat : gui.seats) destroy_seat(*seat);
    gui.seats.clear();
    for (auto& output : gui.outputs) if (output->native) wl_output_release(output->native);
    gui.outputs.clear();
    unref(state.lua, gui.on_outputs);
    if (gui.focus_grabs) hyprland_focus_grab_manager_v1_destroy(gui.focus_grabs);
    if (gui.data) wl_data_device_manager_destroy(gui.data);
    if (gui.text_input) zwp_text_input_manager_v3_destroy(gui.text_input);
    if (gui.cursors) wp_cursor_shape_manager_v1_destroy(gui.cursors);
    if (gui.fractions) wp_fractional_scale_manager_v1_destroy(gui.fractions);
    if (gui.viewporter) wp_viewporter_destroy(gui.viewporter);
    if (gui.layers) zwlr_layer_shell_v1_destroy(gui.layers);
    if (gui.shell) xdg_wm_base_destroy(gui.shell);
    gui.buffers.clear();
    if (gui.shm) wl_shm_destroy(gui.shm);
    if (gui.compositor) wl_compositor_destroy(gui.compositor);
    if (gui.registry) wl_registry_destroy(gui.registry);
    if (gui.display) wl_display_disconnect(gui.display);
    gui.display = nullptr;
    if (gui.text_context) g_object_unref(gui.text_context);
    if (gui.fonts) g_object_unref(gui.fonts);
    if (gui.compose) xkb_compose_table_unref(gui.compose);
    if (gui.xkb) xkb_context_unref(gui.xkb);
}

Gui::~Gui() { close_gui(*this); }

void prepare_gui(InvocationState& state, WaitSet& wait) {
    auto& gui = *state.gui;
    if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
    if (wl_display_dispatch_pending(gui.display) < 0) check_display(gui);
    check_display(gui);
    gui.wait_offset = wait.fds.size();
    wait.fds.push_back({wl_display_get_fd(gui.display), POLLIN, 0});
    gui.transfer_wait_count = gui.transfers.size();
    const auto deadline = [&](Clock::time_point time) {
        if (!wait.deadline || time < *wait.deadline) wait.deadline = time;
    };
    for (const auto& transfer : gui.transfers) {
        wait.fds.push_back({transfer.fd, static_cast<short>(transfer.writing ? POLLOUT : POLLIN), 0});
        deadline(transfer.deadline);
        if (transfer.fd < 0) deadline(Clock::time_point::min());
    }
    if (gui.event_count || gui.outputs_pending) deadline(Clock::time_point::min());
    for (const auto& seat : gui.seats) if (seat->repeat_at) deadline(*seat->repeat_at);
    for (const auto& surface : gui.surfaces) if (surface) {
        if (surface->closed || surface->close_pending || surface->configure_pending || surface->grab_pending ||
            (surface->dirty && surface->configured && !surface->frame && !surface->waiting_buffer))
            deadline(Clock::time_point::min());
        for (const auto& element : surface->elements)
            if (element.image && !element.image_notified && element.image->ready.load(std::memory_order_acquire))
                deadline(Clock::time_point::min());
    }
    if (wl_display_prepare_read(gui.display) == 0) gui.read_prepared = true;
    else deadline(Clock::time_point::min());
    if (wl_display_flush(gui.display) < 0) {
        if (errno == EAGAIN) wait.fds[gui.wait_offset].events |= POLLOUT;
        else {
            if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
            check_display(gui);
            throw std::runtime_error("cannot flush Wayland connection");
        }
    }
}

void read_gui(InvocationState& state, const WaitSet& wait) {
    auto& gui = *state.gui;
    if (gui.wait_offset >= wait.fds.size()) {
        if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
        return;
    }
    const auto events = wait.fds[gui.wait_offset].revents;
    if (gui.read_prepared) {
        if (events & POLLIN) wl_display_read_events(gui.display);
        else wl_display_cancel_read(gui.display);
        gui.read_prepared = false;
    }
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
        check_display(gui); throw std::runtime_error("Wayland display disconnected");
    }
    for (std::size_t i = 0; i < gui.transfer_wait_count; ++i)
        transfer_io(gui.transfers[i], wait.fds[gui.wait_offset + 1 + i].revents);
    if (wl_display_dispatch_pending(gui.display) < 0) check_display(gui);
    check_display(gui);
}

int dispatch_gui(InvocationState& state) {
    if (!state.gui) return 0;
    auto& gui = *state.gui;
    check_display(gui);
    if (std::exchange(gui.outputs_pending, false) && notify(gui, 0, -1, gui.on_outputs) < 0) return -1;
    for (auto& seat : gui.seats) {
        if (seat->repeat_at && Clock::now() >= *seat->repeat_at) {
            enqueue(*seat, key_event(*seat, seat->repeat_key, true, 0));
            seat->repeat_at = Clock::now() + std::chrono::milliseconds(1000 / std::max(1, seat->repeat_rate));
        }
    }
    for (std::size_t i = 0; i < gui.event_count && !state.quit_requested.load(std::memory_order_relaxed); ++i) {
        auto& event = gui.events[i];
        gui.event_seat = find_seat(gui, event.seat);
        gui.event_serial = event.type == Input::Type::key || event.type == Input::Type::button ? event.serial : 0;
        gui.event_surface = event.surface;
        bool consumed = false;
        auto* recipient = find_surface(gui, event.surface);
        if (recipient && recipient->on_input >= 0 && event.type <= Input::Type::blur) {
            const int reference = recipient->on_input;
            lua_pushcfunction(state.lua, traceback);
            const int result = protected_call(state.lua, [&](lua_State* lua) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
                push_handle(lua, {event.surface, 0, -1}, surface_type);
                lua_createtable(lua, 0, 10);
                static const char* kinds[] = {"motion", "leave", "button", "axis", "key", "focus", "blur"};
                lua_pushstring(lua, kinds[static_cast<unsigned>(event.type)]); lua_setfield(lua, -2, "kind");
                char key[128]{};
                if (event.type == Input::Type::key) xkb_keysym_get_name(event.key, key, sizeof(key));
                lua_pushstring(lua, key); lua_setfield(lua, -2, "key");
                lua_pushinteger(lua, event.key); lua_setfield(lua, -2, "button");
                lua_pushinteger(lua, event.seat); lua_setfield(lua, -2, "seat");
                lua_pushnumber(lua, event.x); lua_setfield(lua, -2, "x");
                lua_pushnumber(lua, event.y); lua_setfield(lua, -2, "y");
                lua_pushnumber(lua, event.delta); lua_setfield(lua, -2, "delta");
                lua_pushboolean(lua, event.pressed); lua_setfield(lua, -2, "pressed");
                lua_pushboolean(lua, event.ctrl); lua_setfield(lua, -2, "ctrl");
                lua_pushboolean(lua, event.alt); lua_setfield(lua, -2, "alt");
                lua_pushboolean(lua, event.shift); lua_setfield(lua, -2, "shift");
                lua_pushboolean(lua, event.touch); lua_setfield(lua, -2, "touch");
                if (!find_surface(gui, event.surface) || state.quit_requested.load(std::memory_order_relaxed)) return 0;
                lua_call(lua, 2, 1);
                consumed = event.type == Input::Type::key && lua_toboolean(lua, -1);
                return 0;
            }, 0, 1);
            if (result < 0) {
                gui.event_seat = nullptr; gui.event_serial = 0; gui.event_surface = 0; return -1;
            }
            lua_settop(state.lua, 0);
        }
        const int target = consumed || state.quit_requested.load(std::memory_order_relaxed) ? -1 : process_input(gui, event);
        auto* surface = find_surface(gui, event.surface);
        if (surface && target >= 0 && notify(gui, surface->id, target, surface->elements[static_cast<std::size_t>(target)].callback,
            surface->elements[static_cast<std::size_t>(target)].kind == Kind::button ? "activate" : "change") < 0) {
            gui.event_seat = nullptr; gui.event_serial = 0; gui.event_surface = 0; return -1;
        }
        gui.event_seat = nullptr; gui.event_serial = 0; gui.event_surface = 0;
        for (auto& seat : gui.seats) update_ime(*seat);
    }
    gui.event_count = 0;
    for (std::size_t i = 0; i < gui.transfers.size();) {
        if (gui.transfers[i].fd >= 0) { ++i; continue; }
        auto transfer = std::move(gui.transfers[i]); gui.transfers.erase(gui.transfers.begin() + static_cast<std::ptrdiff_t>(i));
        if (transfer.writing) continue;
        if (transfer.error.empty() && (transfer.data.find('\0') != std::string::npos ||
            !g_utf8_validate(transfer.data.data(), static_cast<gssize>(transfer.data.size()), nullptr)))
            transfer.error = "clipboard is not valid UTF-8 text";
        if (transfer.surface && transfer.error.empty()) {
            auto* surface = find_surface(gui, transfer.surface);
            if (surface && surface->revision == transfer.revision && surface->focused == transfer.element) {
                auto& element = surface->elements[static_cast<std::size_t>(transfer.element)];
                if (element.edit_revision == transfer.edit_revision &&
                    replace_text(*surface, element, std::min(element.cursor, element.anchor), std::max(element.cursor, element.anchor), transfer.data) &&
                    notify(gui, surface->id, transfer.element, element.callback, "change") < 0) return -1;
            }
        } else if (transfer.callback >= 0) {
            lua_pushcfunction(state.lua, traceback);
            const int result = protected_call(state.lua, [&](lua_State* lua) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, transfer.callback);
                if (transfer.error.empty()) lua_pushlstring(lua, transfer.data.data(), transfer.data.size()); else lua_pushnil(lua);
                if (transfer.error.empty()) lua_pushnil(lua); else lua_pushlstring(lua, transfer.error.data(), transfer.error.size());
                if (!state.closing && !state.quit_requested.load(std::memory_order_relaxed)) lua_call(lua, 2, 0);
                return 0;
            }, 0, 1);
            unref(state.lua, transfer.callback);
            if (result < 0) return -1;
            lua_settop(state.lua, 0);
        }
    }
    for (auto& surface : gui.surfaces) {
        if (!surface || surface->closed) continue;
        const auto id = surface->id;
        if (std::exchange(surface->close_pending, false)) {
            if (notify(gui, id, -1, surface->on_close) < 0) return -1;
            close_surface(*surface); continue;
        }
        if (std::exchange(surface->configure_pending, false) && notify(gui, id, -1, surface->on_configure) < 0) return -1;
        if (surface->closed) continue;
        if (std::exchange(surface->grab_pending, false) && notify(gui, id, -1, surface->on_grab_cleared) < 0) return -1;
        const auto revision = surface->revision;
        layout(*surface);
        for (std::size_t i = 0; !surface->closed && surface->revision == revision && i < surface->elements.size(); ++i) {
            auto& element = surface->elements[i];
            if (element.image && !element.image->icon.empty()) {
                const auto pixels = static_cast<int>(std::clamp(std::ceil(std::max(1.0, element.box.w) * surface->scale), 1.0, 4096.0));
                if (element.image->pixels != pixels) {
                    auto image = std::make_shared<Image>(gui.images);
                    image->icon = element.image->icon; image->theme = element.image->theme; image->pixels = pixels;
                    element.image = std::move(image); element.image_notified = false;
                }
            }
            if (element.image && !element.image->submitted) gui.images.submit(element.image);
            if (!element.image || element.image_notified || !element.image->ready.load(std::memory_order_acquire)) continue;
            element.image_notified = true; dirty(*surface);
            const auto image = element.image;
            if (notify(gui, id, static_cast<int>(i), element.callback, "loaded", &image->error) < 0) return -1;
        }
    }
    for (auto& seat : gui.seats) update_ime(*seat);
    for (auto& surface : gui.surfaces) if (surface && surface->closed) destroy_surface(*surface);
    for (auto& surface : gui.surfaces) if (surface && surface->closed) surface.reset();
    std::erase_if(gui.buffers, [&](const auto& buffer) { return !buffer->busy && !find_surface(gui, buffer->surface); });
    for (auto& surface : gui.surfaces)
        if (surface && !state.quit_requested.load(std::memory_order_relaxed)) render(*surface);
    return 0;
}

void open_gui(lua_State* lua) {
    luaL_newmetatable(lua, surface_type);
    const luaL_Reg surfaces[] = {
        {"close", gui_call<surface_close>}, {"state", gui_call<surface_state>},
        {"set_content", gui_call<set_content>}, {"find", gui_call<find_element>},
        {"grab_focus", gui_call<grab_focus>}, {"release_focus", gui_call<release_focus>},
        {"set_input_region", gui_call<input_region>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, surfaces, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, gui_call<surface_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks surface"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    luaL_newmetatable(lua, element_type);
    const luaL_Reg elements[] = {
        {"set", gui_call<set_element>}, {"state", gui_call<element_state>}, {"focus", gui_call<focus_element>},
        {"replace_children", gui_call<replace_children>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, elements, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushliteral(lua, "hyprworks element"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    const luaL_Reg functions[] = {
        {"layer", gui_call<create_surface<1>>}, {"window", gui_call<create_surface<0>>}, {"popup", gui_call<create_surface<2>>},
        {"outputs", gui_call<outputs>}, {"on_outputs", gui_call<on_outputs>}, {"capabilities", gui_call<capabilities>},
        {"clipboard_read", gui_call<clipboard_read>}, {"clipboard_write", gui_call<clipboard_write>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
