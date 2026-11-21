#include "../runtime/invocation.hpp"
#include "gui.hpp"

#include <algorithm>
#include <cerrno>
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

using Clock = std::chrono::steady_clock;

static Seat* find_seat(Gui& gui, std::uint32_t id) {
    for (auto& seat : gui.seats) if (seat->id == id) return seat.get();
    return nullptr;
}

static std::uint64_t surface_id(wl_surface* native) {
    auto* surface = native ? static_cast<Surface*>(wl_surface_get_user_data(native)) : nullptr;
    return surface ? surface->id : 0;
}

void enqueue(Seat& seat, Input event) {
    auto& gui = *seat.gui;
    if (gui.event_count == gui.events.size()) {
        gui.error = "Wayland input queue limit reached";
        return;
    }
    event.seat = seat.id;
    event.keyboard_generation = seat.keyboard_generation;
    if (const auto* surface = find_surface(gui, event.surface)) event.generation = surface->input_generation;
    gui.events[gui.event_count++] = std::move(event);
}

static Input key_event(Seat& seat, std::uint32_t key, bool pressed, std::uint32_t serial) {
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
        seat.keyboard_tree.fill(0);
        std::size_t depth = 0;
        for (auto* surface = native ? static_cast<Surface*>(wl_surface_get_user_data(native)) : nullptr;
            surface; surface = find_surface(*seat.gui, surface->parent)) {
            seat.keyboard_tree[depth++] = surface->id;
            surface->focus_seen = true;
        }
        enqueue(seat, {.type = Input::Type::focus, .surface = seat.keyboard_surface});
    },
    .leave = [](void* data, wl_keyboard*, std::uint32_t, wl_surface*) {
        auto& seat = *static_cast<Seat*>(data);
        ++seat.keyboard_generation;
        seat.keyboard_tree.fill(0);
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
        enqueue(seat, {.type = Input::Type::axis, .surface = seat.pointer_surface,
            .x = seat.x, .y = seat.y, .delta = wl_fixed_to_double(value), .horizontal = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL});
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
            ++seat.keyboard_generation;
            seat.keyboard_tree.fill(0);
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

int dispatch_scroll(Surface& surface) {
    while (!surface.closed && !surface.gui->owner.requests.load(std::memory_order_relaxed)) {
        layout(surface);
        const auto found = std::ranges::find_if(surface.elements, [](const auto& element) { return element.scroll_origin.has_value(); });
        if (found == surface.elements.end()) break;
        const double origin = *std::exchange(found->scroll_origin, {});
        if (origin == found->scroll) continue;
        const int index = static_cast<int>(found - surface.elements.begin());
        if (notify(*surface.gui, surface.id, index, found->callback, "change") < 0) return -1;
    }
    return 0;
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

static int adjacent_cursor(Element& element, int position, int direction) {
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

static void copy_selection(Seat& seat, const std::string& text) {
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

void paste(Seat& seat, Surface* surface, int callback) {
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

static int clipboard_read(lua_State* lua) {
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

static int clipboard_write(lua_State* lua) {
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

static void position_input_cursor(Surface& surface, Element& element, double x, double y, bool select) {
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
    if (!surface || surface->close_pending) return -1;
    auto& s = *surface;
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
    if (event.type == Input::Type::focus) {
        if (s.focused >= 0) reveal_focus(s, true);
        s.dirty = true; return -1;
    }
    if (event.generation != s.input_generation) return -1;
    if (event.type == Input::Type::key && event.pressed && event.serial && event.key == XKB_KEY_Escape) {
        for (auto* current = &s; current; current = find_surface(gui, current->parent)) {
            if (!current->dismiss_escape) continue;
            request_surface_close(*current, "escape");
            return -1;
        }
    }
    layout(s);
    if (event.type == Input::Type::axis) {
        auto* seat = find_seat(gui, event.seat);
        if (!seat) return -1;
        for (int i = static_cast<int>(s.elements.size()) - 1; i >= 0; --i) {
            auto& element = s.elements[static_cast<std::size_t>(i)];
            if (element.kind == Kind::scroll && element.enabled && (!event.horizontal || element.horizontal) &&
                element.clip.contains(event.x, event.y)) {
                if (!element.scroll_origin) element.scroll_origin = element.scroll;
                element.scroll = std::max(0.0, element.scroll + event.delta);
                dirty(s); layout(s); break;
            }
        }
        return -1;
    }
    int target = -1;
    for (int i = static_cast<int>(s.elements.size()) - 1; i >= 0; --i)
        if (focusable(s.elements[static_cast<std::size_t>(i)]) && s.elements[static_cast<std::size_t>(i)].clip.contains(event.x, event.y)) { target = i; break; }
    if (event.type == Input::Type::button && event.key == BTN_LEFT) {
        if (event.pressed) {
            if (s.pressed >= 0 && !owns_press) return -1;
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
            if (focusable(s.elements[static_cast<std::size_t>(next)])) {
                focus(s, next);
                reveal_focus(s, true);
                break;
            }
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

int dispatch_input(Gui& gui) {
    auto& state = gui.owner;
    for (auto& seat : gui.seats) {
        if (seat->repeat_at && Clock::now() >= *seat->repeat_at) {
            enqueue(*seat, key_event(*seat, seat->repeat_key, true, 0));
            seat->repeat_at = Clock::now() + std::chrono::milliseconds(1000 / std::max(1, seat->repeat_rate));
        }
    }
    for (std::size_t i = 0; i < gui.event_count && !state.requests.load(std::memory_order_relaxed); ++i) {
        auto& event = gui.events[i];
        const bool lifecycle = event.type == Input::Type::leave ||
            event.type == Input::Type::focus || event.type == Input::Type::blur || event.type == Input::Type::ime_leave;
        auto* recipient = find_surface(gui, event.surface);
        if (!recipient || recipient->close_pending || (!lifecycle && event.generation != recipient->input_generation)) continue;
        auto* event_seat = find_seat(gui, event.seat);
        if (!lifecycle && (!event_seat || ((event.type == Input::Type::key || event.type == Input::Type::ime) &&
            event.keyboard_generation != event_seat->keyboard_generation))) continue;
        gui.event_seat = event_seat;
        gui.event_serial = event.type == Input::Type::key || event.type == Input::Type::button ? event.serial : 0;
        gui.event_surface = event.surface;
        if (lifecycle) process_input(gui, event);
        bool consumed = false;
        if (event.pressed && (event.type == Input::Type::key || event.type == Input::Type::button)) {
            const bool visible = event.type == Input::Type::key;
            if (recipient->focus_visible != visible) {
                recipient->focus_visible = visible;
                recipient->dirty = true;
            }
        }
        if (recipient->on_input >= 0 && event.type <= Input::Type::blur) {
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
                lua_pushboolean(lua, event.horizontal); lua_setfield(lua, -2, "horizontal");
                const auto* live = find_surface(gui, event.surface);
                if (!live || live->close_pending || (!lifecycle && event.generation != live->input_generation) ||
                    state.requests.load(std::memory_order_relaxed)) return 0;
                lua_call(lua, 2, 1);
                consumed = lua_toboolean(lua, -1);
                return 0;
            }, 0, 1);
            if (result < 0) {
                gui.event_seat = nullptr; gui.event_serial = 0; gui.event_surface = 0; return -1;
            }
            lua_settop(state.lua, 0);
        }
        if (consumed && (event.type == Input::Type::motion ||
            (event.type == Input::Type::button && event.key == BTN_LEFT))) {
            auto* surface = find_surface(gui, event.surface);
            if (surface && surface->press_seat == event.seat && surface->press_touch == event.touch)
                surface->pressed = -1;
        }
        const int target = lifecycle || consumed || state.requests.load(std::memory_order_relaxed) ?
            -1 : process_input(gui, event);
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
            if (surface && !surface->close_pending && surface->revision == transfer.revision && surface->focused == transfer.element) {
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
                if (!state.closing && !state.requests.load(std::memory_order_relaxed)) lua_call(lua, 2, 0);
                return 0;
            }, 0, 1);
            unref(state.lua, transfer.callback);
            if (result < 0) return -1;
            lua_settop(state.lua, 0);
        }
    }
    return 0;
}

void register_input(lua_State* lua) {
    const luaL_Reg functions[] = {
        {"clipboard_read", operation_call<clipboard_read>}, {"clipboard_write", operation_call<clipboard_write>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
