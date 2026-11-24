#include "../runtime/invocation.hpp"
#include "gui.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace hyprworks::lua {

Surface* find_surface(Gui& gui, std::uint64_t id) {
    for (auto& surface : gui.surfaces)
        if (surface && surface->id == id && !surface->closed) return surface.get();
    return nullptr;
}

SurfaceFocus surface_focus(const Surface& surface) {
    SurfaceFocus result;
    if (surface.closed || surface.close_pending) return result;
    auto& gui = *surface.gui;
    for (const auto& seat : gui.seats) {
        result.focused |= seat->keyboard_surface == surface.id;
        // Native keyboard leave ends the ancestry lifetime, including retired popups.
        result.within |= seat->keyboard_surface == surface.id ||
            std::ranges::find(seat->keyboard_tree, surface.id) != seat->keyboard_tree.end();
    }
    return result;
}

void focus(Surface& surface, int index) {
    assert(index == -1 || (index >= 0 && static_cast<std::size_t>(index) < surface.elements.size() &&
        focusable(surface.elements[static_cast<std::size_t>(index)])));
    if (surface.focused == index) return;
    surface.key_pressed = -1;
    if (surface.focused >= 0) {
        auto& previous = surface.elements[static_cast<std::size_t>(surface.focused)];
        const bool composing = !previous.preedit.empty();
        previous.preedit.clear(); ++previous.edit_revision;
        invalidate_element(surface, surface.focused, composing ? shaping_changed : paint_changed);
    }
    surface.focused = index;
    if (index >= 0) ++surface.elements[static_cast<std::size_t>(index)].edit_revision;
    for (auto& seat : surface.gui->seats)
        if (seat->keyboard_surface == surface.id && seat->compose) xkb_compose_state_reset(seat->compose);
    invalidate_element(surface, index);
}

namespace {

xdg_positioner* popup_positioner(const Surface& parent, const PopupPlacement& placement) {
    auto* position = xdg_wm_base_create_positioner(parent.gui->shell);
    if (!position) throw std::runtime_error("cannot create popup positioner");
    xdg_positioner_set_size(position, placement.width, placement.height);
    xdg_positioner_set_anchor_rect(position, std::clamp(placement.x, 0, parent.width - 1),
        std::clamp(placement.y, 0, parent.height - 1), 1, 1);
    xdg_positioner_set_anchor(position, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(position, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(position,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
    xdg_positioner_set_reactive(position);
    xdg_positioner_set_parent_size(position, parent.width, parent.height);
    if (parent.xdg) xdg_positioner_set_parent_configure(position, parent.configure_serial);
    return position;
}

Surface& root_surface(Surface& surface) {
    auto* root = &surface;
    while (root->parent) {
        const auto parent = std::ranges::find_if(surface.gui->surfaces,
            [&](const auto& candidate) { return candidate && candidate->id == root->parent; });
        // Parents remain allocated until every descendant has been destroyed.
        assert(parent != surface.gui->surfaces.end());
        root = parent->get();
    }
    return *root;
}

void clear_elements(lua_State* lua, std::vector<Element>& elements) {
    for (auto& element : elements) {
        if (element.layout) g_object_unref(element.layout);
        unref(lua, element.callback);
    }
    elements.clear();
}

}

Surface::Surface(Gui& owner) : gui(&owner), id(++owner.next_surface) {}

Surface::~Surface() {
    if (native && parent) {
        auto& root = root_surface(*this);
        if (root.grab) {
            hyprland_focus_grab_v1_remove_surface(root.grab, native);
            hyprland_focus_grab_v1_commit(root.grab);
        }
    }
    if (frame) wl_callback_destroy(frame);
    if (popup) xdg_popup_destroy(popup);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (xdg) xdg_surface_destroy(xdg);
    if (layer) zwlr_layer_surface_v1_destroy(layer);
    if (fractional) wp_fractional_scale_v1_destroy(fractional);
    if (viewport) wp_viewport_destroy(viewport);
    if (native) wl_surface_destroy(native);
    auto* lua = gui->owner.lua;
    clear_elements(lua, elements);
    for (auto* reference : {&on_configure, &on_close, &on_grab_cleared, &on_input, &on_focus_change})
        unref(lua, *reference);
}

void retire_surface(std::unique_ptr<Surface>& slot) {
    if (slot->grab) hyprland_focus_grab_v1_destroy(std::exchange(slot->grab, nullptr));
    for (auto& child : slot->gui->surfaces)
        if (child && child->parent == slot->id) retire_surface(child);
    slot.reset();
}

void close_surface(Surface& surface) {
    if (surface.closed) return;
    cancel_surface_input(surface);
    surface.closed = true;
    surface.gui->focus_pending = true;
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    surface.grab_pending = false;
    for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id) close_surface(*child);
}

void cancel_surface_input(Surface& surface, bool descendants) {
    ++surface.input_generation;
    surface.pressed = -1;
    surface.key_pressed = -1;
    for (auto& element : surface.elements) if (element.kind == Kind::input) {
        const bool composing = !element.preedit.empty();
        element.preedit.clear(); ++element.edit_revision;
        invalidate_element(surface, static_cast<int>(&element - surface.elements.data()), composing ? shaping_changed : paint_changed);
    }
    for (auto& seat : surface.gui->seats) if (seat->keyboard_surface == surface.id) {
        seat->repeat_at.reset();
        if (seat->compose) xkb_compose_state_reset(seat->compose);
    }
    if (descendants) for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id && !child->closed) cancel_surface_input(*child, true);
}

int dispatch_close(Surface& surface) {
    if (surface.closed) return 0;
    auto& gui = *surface.gui;
    for (auto& child : gui.surfaces)
        if (child && child->parent == surface.id && child->close_pending && dispatch_close(*child) < 0) return -1;
    if (notify_surface(surface, surface.on_close) < 0) return -1;
    close_surface(surface);
    return 0;
}

void reposition_popup(Surface& surface) {
    if (!surface.popup_reposition_pending || !surface.mapped || surface.closed || surface.close_pending) return;
    auto* parent = find_surface(*surface.gui, surface.parent);
    assert(parent && surface.popup);
    auto* position = popup_positioner(*parent, surface.popup_placement);
    xdg_popup_reposition(surface.popup, position, 0);
    xdg_positioner_destroy(position);
    surface.popup_reposition_pending = false;
}

void update_scale(Surface& surface) {
    if (surface.fractional) return;
    int scale = 1;
    for (const auto& output : surface.gui->outputs)
        if (std::ranges::find(surface.outputs, output->id) != surface.outputs.end()) scale = std::max(scale, output->state.scale);
    if (surface.scale != scale) {
        surface.scale = scale; dirty(surface); surface.configure_pending = true;
        if (surface.frame) wl_callback_destroy(std::exchange(surface.frame, nullptr));
    }
}

Gui& gui_state(lua_State* lua) {
    auto& state = invocation(lua);
    if (state.closing) throw std::runtime_error("runtime is stopping");
    if (!state.gui) throw std::runtime_error("GUI is unavailable in headless mode");
    return *state.gui;
}

static Handle handle(lua_State* lua, int index, const char* type) {
    return *static_cast<Handle*>(luaL_checkudata(lua, index, type));
}

static Surface& live_surface(Gui& gui, Handle value) {
    auto* surface = find_surface(gui, value.surface);
    if (!surface) throw std::runtime_error("surface is closed");
    if (value.element >= 0 && value.revision != surface->revision)
        throw std::runtime_error("element belongs to a replaced tree");
    return *surface;
}

void push_handle(lua_State* lua, Handle value, const char* type) {
    *static_cast<Handle*>(lua_newuserdatauv(lua, sizeof(Handle), 0)) = value;
    luaL_setmetatable(lua, type);
}

void request_surface_close(Surface& surface, const char* reason) {
    if (surface.closed || surface.close_pending) return;
    cancel_surface_input(surface);
    surface.close_pending = true;
    surface.close_reason = reason;
    for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id) request_surface_close(*child, "parent_closed");
}

namespace {

void set_configure(Surface& surface, std::int64_t width, std::int64_t height) {
    if (surface.closed || surface.close_pending) return;
    if (width < 1 || height < 1 || width > 16384 || height > 16384) {
        surface.gui->error = "configured surface dimensions outside 1..16384";
        return;
    }
    const bool initial = !surface.configured;
    const bool resized = surface.width != width || surface.height != height;
    if (resized || (surface.parent &&
        (surface.popup_x != surface.pending_popup_x || surface.popup_y != surface.pending_popup_y)))
        cancel_surface_input(surface, true);
    surface.width = static_cast<int>(width); surface.height = static_cast<int>(height);
    surface.configured = true; surface.configure_pending = true;
    if (surface.popup) {
        surface.popup_x = surface.pending_popup_x;
        surface.popup_y = surface.pending_popup_y;
    }
    if (resized) {
        if (surface.frame) wl_callback_destroy(std::exchange(surface.frame, nullptr));
        for (auto& child : surface.gui->surfaces)
            if (child && child->parent == surface.id && !child->closed && !child->close_pending)
                child->popup_reposition_pending = true;
    }
    if (initial && surface.parent) {
        auto& root = root_surface(surface);
        if (root.grab) {
            hyprland_focus_grab_v1_add_surface(root.grab, surface.native);
            hyprland_focus_grab_v1_commit(root.grab);
        }
    }
    dirty(surface);
}

const xdg_surface_listener xdg_listener{
    .configure = [](void* data, xdg_surface* xdg, std::uint32_t serial) {
        auto& surface = *static_cast<Surface*>(data);
        xdg_surface_ack_configure(xdg, serial);
        surface.configure_serial = serial;
        set_configure(surface, surface.pending_width, surface.pending_height);
    }
};
const xdg_toplevel_listener toplevel_listener{
    .configure = [](void* data, xdg_toplevel*, int width, int height, wl_array*) {
        auto& surface = *static_cast<Surface*>(data);
        if (width > 0) surface.pending_width = width;
        if (height > 0) surface.pending_height = height;
    },
    .close = [](void* data, xdg_toplevel*) { request_surface_close(*static_cast<Surface*>(data)); },
    .configure_bounds = [](void*, xdg_toplevel*, int, int) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};
const xdg_popup_listener popup_listener{
    .configure = [](void* data, xdg_popup*, int x, int y, int width, int height) {
        auto& surface = *static_cast<Surface*>(data);
        surface.pending_popup_x = x; surface.pending_popup_y = y;
        surface.pending_width = width; surface.pending_height = height;
    },
    .popup_done = [](void* data, xdg_popup*) { request_surface_close(*static_cast<Surface*>(data)); },
    .repositioned = [](void*, xdg_popup*, std::uint32_t) {},
};
const zwlr_layer_surface_v1_listener layer_listener{
    .configure = [](void* data, zwlr_layer_surface_v1* layer, std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
        auto& surface = *static_cast<Surface*>(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        set_configure(surface, width ? std::int64_t(width) : surface.pending_width,
            height ? std::int64_t(height) : surface.pending_height);
    },
    .closed = [](void* data, zwlr_layer_surface_v1*) { request_surface_close(*static_cast<Surface*>(data)); }
};

const wl_surface_listener surface_listener{
    .enter = [](void* data, wl_surface*, wl_output* native) {
        auto& surface = *static_cast<Surface*>(data);
        protocol(*surface.gui, [&] {
            for (const auto& output : surface.gui->outputs)
                if (output->native == native && std::ranges::find(surface.outputs, output->id) == surface.outputs.end())
                    surface.outputs.push_back(output->id);
            surface.configure_pending = true;
            update_scale(surface);
        });
    },
    .leave = [](void* data, wl_surface*, wl_output* native) {
        auto& surface = *static_cast<Surface*>(data);
        for (const auto& output : surface.gui->outputs) if (output->native == native) std::erase(surface.outputs, output->id);
        surface.configure_pending = true;
        update_scale(surface);
    },
    .preferred_buffer_scale = [](void*, wl_surface*, int) {},
    .preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {},
};
const wp_fractional_scale_v1_listener scale_listener{
    .preferred_scale = [](void* data, wp_fractional_scale_v1*, std::uint32_t scale) {
        auto& surface = *static_cast<Surface*>(data);
        if (!scale || scale > 960) { surface.gui->error = "unsupported fractional scale"; return; }
        if (surface.scale == scale / 120.0) return;
        surface.scale = scale / 120.0;
        if (surface.frame) wl_callback_destroy(std::exchange(surface.frame, nullptr));
        surface.configure_pending = true; dirty(surface);
    }
};
bool container(Kind kind) {
    return kind == Kind::row || kind == Kind::column || kind == Kind::scroll || kind == Kind::box ||
        kind == Kind::button || kind == Kind::toggle || kind == Kind::slider;
}
template<class T>
struct Property { T Element::*member; T value; Damage damage; };

struct ElementPatch {
    lua_State* lua;
    std::vector<Property<double>> numbers;
    std::vector<Property<bool>> flags;
    std::vector<Property<int>> enums;
    std::vector<Property<std::uint32_t>> colors;
    std::vector<Property<std::string>> strings;
    std::shared_ptr<Image> image;
    int callback = LUA_NOREF;
    unsigned specified = 0;
    explicit ElementPatch(lua_State* state) : lua(state) {}
    ~ElementPatch() { unref(lua, callback); }
};
constexpr unsigned text_specified = 1, value_specified = 2, scroll_specified = 4;

void read_properties(lua_State* lua, int table, ElementPatch& patch, Gui& gui, Kind kind,
                     bool creating, std::size_t remaining) {
    table = lua_absindex(lua, table);
    const auto present = [&](int index, const char* name) {
        field(lua, index, name);
        if (!lua_isnil(lua, -1)) return true;
        lua_pop(lua, 1); return false;
    };
    const auto string = [&](int index, const char* name, std::string Element::*member,
                            std::size_t limit, Damage damage) {
        if (!present(index, name)) return false;
        std::size_t size;
        const char* data = check_string(lua, -1, &size);
        if (size > remaining || size > limit || !valid_text({data, size}))
            luaL_error(lua, "%s exceeds its text budget or is invalid UTF-8", name);
        remaining -= size;
        patch.strings.push_back({member, std::string(data, size), damage});
        lua_pop(lua, 1); return true;
    };
    const auto number = [&](const char* name, double Element::*member, double min, double max, Damage damage) {
        if (!present(table, name)) return false;
        const auto value = check_number(lua, -1);
        if (!(value >= min && value <= max)) luaL_error(lua, "%s is out of range", name);
        patch.numbers.push_back({member, value, damage});
        lua_pop(lua, 1); return true;
    };
    const auto flag = [&](int index, const char* name, bool Element::*member, Damage damage) {
        if (!present(index, name)) return false;
        luaL_checktype(lua, -1, LUA_TBOOLEAN);
        patch.flags.push_back({member, lua_toboolean(lua, -1) != 0, damage});
        lua_pop(lua, 1); return true;
    };
    const auto color = [&](const char* name, std::uint32_t Element::*member) {
        if (!present(table, name)) return;
        std::size_t length;
        const char* data = check_string(lua, -1, &length);
        if ((length != 7 && length != 9) || data[0] != '#') luaL_error(lua, "%s must be #RRGGBB or #RRGGBBAA", name);
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(data + 1, data + length, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != data + length) luaL_error(lua, "invalid color");
        patch.colors.push_back({member, length == 7 ? (value << 8) | 255 : value, paint_changed});
        lua_pop(lua, 1);
    };
    const auto alignment = [&](const char* name, int Element::*member, const char* const* choices, Damage damage) {
        if (!present(table, name)) return;
        patch.enums.push_back({member, check_option(lua, -1, nullptr, choices), damage});
        lua_pop(lua, 1);
    };
    if (creating) {
        string(table, "id", &Element::id, 128, unchanged);
        flag(table, "thumb", &Element::thumb, unchanged);
        if (kind == Kind::slider) string(table, "scroll_target", &Element::scroll_target, 128, unchanged);
    }
    field(lua, table, "accessibility");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        static const char* roles[] = {"auto", "group", "label", "image", "button", "checkbox", "slider",
            "textbox", "heading", "link", "list", "list_item", "menu", "menu_item", nullptr};
        field(lua, -1, "role");
        if (!lua_isnil(lua, -1)) patch.strings.push_back({&Element::accessible_role, roles[check_option(lua, -1, nullptr, roles)], unchanged});
        lua_pop(lua, 1);
        string(-1, "name", &Element::accessible_name, max_text, unchanged);
        string(-1, "description", &Element::accessible_description, max_text, unchanged);
        flag(-1, "hidden", &Element::accessible_hidden, unchanged);
    }
    lua_pop(lua, 1);
    number("width", &Element::width, 0, 16384, geometry_changed);
    number("height", &Element::height, 0, 16384, geometry_changed);
    number("x", &Element::x, -16384, 16384, placement_changed);
    number("y", &Element::y, -16384, 16384, placement_changed);
    static const char* horizontal_alignments[] = {"stretch", "left", "center", "right", nullptr};
    static const char* vertical_alignments[] = {"stretch", "top", "center", "bottom", nullptr};
    static const char* text_alignments[] = {"left", "center", "right", nullptr};
    alignment("halign", &Element::halign, horizontal_alignments, geometry_changed);
    alignment("valign", &Element::valign, vertical_alignments, placement_changed);
    number("grow", &Element::grow, 0, 1024, geometry_changed);
    number("padding", &Element::padding, 0, 1024, geometry_changed);
    if (kind == Kind::row || kind == Kind::column || kind == Kind::scroll)
        number("gap", &Element::gap, 0, 1024, geometry_changed);
    number("radius", &Element::radius, 0, 1024, paint_changed);
    number("border_width", &Element::border_width, 0, 1024, paint_changed);
    color("border_color", &Element::border_color);
    color("background", &Element::background);
    flag(table, "enabled", &Element::enabled, paint_changed);
    const bool control = kind == Kind::button || kind == Kind::input || kind == Kind::toggle || kind == Kind::slider;
    if (control) {
        color("focus_color", &Element::focus_color);
        number("focus_width", &Element::focus_width, 0, 1024, paint_changed);
    }
    if (kind == Kind::text || (control && kind != Kind::slider)) {
        if (string(table, "text", &Element::text, max_text, shaping_changed)) patch.specified |= text_specified;
        if (string(table, "font", &Element::font, 256, shaping_changed)) {
            auto* font = pango_font_description_from_string(patch.strings.back().value.c_str());
            const int size = pango_font_description_get_size(font);
            const bool family = pango_font_description_get_family(font) != nullptr;
            pango_font_description_free(font);
            if (!family || size < PANGO_SCALE || size > 256 * PANGO_SCALE) luaL_error(lua, "font requires a family and size in 1..256");
        }
        alignment("align", &Element::align, text_alignments, shaping_changed);
        color("color", &Element::color);
        if (kind != Kind::input) {
            flag(table, "wrap", &Element::wrap, shaping_changed);
            flag(table, "ellipsize", &Element::ellipsize, shaping_changed);
        }
    }
    if (kind == Kind::input) {
        flag(table, "password", &Element::password, shaping_changed);
        string(table, "placeholder", &Element::placeholder, max_text, shaping_changed);
        if (string(table, "mask", &Element::mask, 4, shaping_changed) && g_utf8_strlen(patch.strings.back().value.c_str(), -1) != 1)
            luaL_error(lua, "mask requires one UTF-8 character");
        number("caret_width", &Element::caret_width, 0, 1024, paint_changed);
        color("selection_color", &Element::selection_color);
        color("caret_color", &Element::caret_color);
        color("placeholder_color", &Element::placeholder_color);
    }
    if ((kind == Kind::slider || kind == Kind::toggle) &&
        number("value", &Element::value, 0, 1, kind == Kind::slider ? placement_changed : paint_changed))
        patch.specified |= value_specified;
    if (kind == Kind::slider) {
        number("step", &Element::step, 0, 1, unchanged);
        flag(table, "jump", &Element::jump, unchanged);
    }
    if (kind == Kind::slider || kind == Kind::scroll) {
        const bool supplied = flag(table, "horizontal", &Element::horizontal, geometry_changed);
        if (creating && !supplied) luaL_error(lua, "horizontal is required");
    }
    if (kind == Kind::scroll && number("scroll", &Element::scroll, 0, 33554432, placement_changed)) patch.specified |= scroll_specified;
    if (control || kind == Kind::image || kind == Kind::scroll)
        callback_field(lua, table, kind == Kind::button ? "on_activate" : kind == Kind::image ? "on_loaded" : "on_change", patch.callback);
    if (kind == Kind::image) {
        field(lua, table, "source");
        if (auto* image = static_cast<std::shared_ptr<Image>*>(luaL_testudata(lua, -1, captured_image_type))) {
            patch.image = *image;
        } else if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TTABLE);
            patch.image = std::make_shared<Image>(gui.images);
            auto& image = *patch.image;
            if (text_field(lua, -1, "path", image.path, 4096)) {
                image.pixels = static_cast<int>(integer_field(lua, -1, "pixels", 0, 0, 4096));
            } else if (text_field(lua, -1, "icon", image.icon, 256)) {
                text_field(lua, -1, "theme", image.theme, 256);
                if (image.theme.empty()) luaL_error(lua, "icon source requires a theme");
                image.icon_size = static_cast<int>(integer_field(lua, -1, "pixels", std::nullopt, 1, 4096));
                image.pixels = image.icon_size;
            } else {
                field(lua, -1, "data");
                std::size_t length; const char* bytes = check_string(lua, -1, &length);
                image.channels = static_cast<int>(integer_field(lua, -2, "channels", 0, 0, 4));
                if (image.channels) {
                    image.width = static_cast<int>(integer_field(lua, -2, "width", std::nullopt, 1, 4096));
                    image.height = static_cast<int>(integer_field(lua, -2, "height", std::nullopt, 1, 4096));
                } else image.pixels = static_cast<int>(integer_field(lua, -2, "pixels", 0, 0, 4096));
                image.set_data({bytes, length});
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);
    }
}

Damage apply_properties(Element& element, ElementPatch& patch, std::size_t& bytes) {
    auto total = bytes;
    bool has_font = !element.font.empty(), has_text = !element.text.empty(), has_mask = !element.mask.empty();
    bool password = element.password, edit = false;
    for (const auto& [member, value, damage] : patch.strings) {
        total = total - (element.*member).size() + value.size();
        if (member == &Element::font) has_font = true;
        if (member == &Element::mask) has_mask = true;
        if (member == &Element::text) { has_text = !value.empty(); edit = value != element.text; }
    }
    for (const auto& prop : patch.flags) if (prop.member == &Element::password) password = prop.value;
    if (password && !has_mask) throw std::runtime_error("password input requires a mask");
    if ((has_text || element.kind == Kind::input) && !has_font) throw std::runtime_error("text requires an explicit font");
    if (patch.image) {
        const auto metadata = [](const Image& i) { return i.icon.size() + i.path.size() + i.theme.size(); };
        total = total - (element.image ? metadata(*element.image) : 0) + metadata(*patch.image);
    }
    if (total > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    bytes = total;
    Damage damage = unchanged;
    if (edit || password != element.password) { ++element.edit_revision; element.preedit.clear(); }
    for (auto& prop : patch.strings) {
        auto& current = element.*prop.member;
        if (current == prop.value) continue;
        if (prop.member == &Element::font && element.layout) g_object_unref(std::exchange(element.layout, nullptr));
        current = std::move(prop.value);
        damage = std::max(damage, prop.damage);
    }
    patch.strings.clear();
    const auto apply = [&](auto& changes) {
        for (auto& prop : changes) {
            auto& current = element.*prop.member;
            if (current != prop.value) { current = std::move(prop.value); damage = std::max(damage, prop.damage); }
        }
        changes.clear();
    };
    apply(patch.numbers); apply(patch.flags); apply(patch.enums); apply(patch.colors);
    if (edit) element.cursor = element.anchor = static_cast<int>(element.text.size());
    if (patch.specified & scroll_specified) element.scroll_origin.reset();
    if (patch.callback >= 0) { unref(patch.lua, element.callback); element.callback = std::exchange(patch.callback, LUA_NOREF); }
    if (patch.image) { element.image = std::move(patch.image); element.image_notified = false; damage = std::max(damage, geometry_changed); }
    return damage;
}

struct ParsedTree {
    lua_State* lua;
    std::vector<Element> elements;
    std::vector<unsigned> specified;
    ElementPatch patch{lua};
    std::size_t bytes = 0;
    ~ParsedTree() { clear_elements(lua, elements); }
};

void parse_tree(lua_State* lua, int table, ParsedTree& tree, Gui& gui, int parent, unsigned depth) {
    if (!lua_checkstack(lua, 12)) luaL_error(lua, "cannot grow Lua parsing stack");
    if (depth > 32 || tree.elements.size() == 2048) luaL_error(lua, "tree limit reached (2048 elements, depth 32)");
    luaL_checktype(lua, table, LUA_TTABLE);
    table = lua_absindex(lua, table);
    static const char* kinds[] = {"row", "column", "scroll", "box", "text", "image", "button", "toggle", "slider", "input", nullptr};
    const auto kind = static_cast<Kind>(enum_field(lua, table, "kind", nullptr, kinds));
    const int index = static_cast<int>(tree.elements.size());
    tree.elements.emplace_back();
    auto& element = tree.elements.back(); element.kind = kind; element.parent = parent;
    tree.patch.specified = 0;
    read_properties(lua, table, tree.patch, gui, kind, true, max_content - tree.bytes);
    tree.specified.push_back(tree.patch.specified);
    apply_properties(element, tree.patch, tree.bytes);
    if (container(kind)) {
        field(lua, table, "children");
        if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1);
            if (count > 2048 - tree.elements.size()) luaL_error(lua, "too many children");
            for (std::size_t child = 1; child <= count; ++child) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(child));
                parse_tree(lua, -1, tree, gui, index, depth + 1); lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);
    }
    tree.elements[static_cast<std::size_t>(index)].end = static_cast<int>(tree.elements.size());
}

std::size_t element_text_bytes(const Element& element) {
    return element.id.size() + element.text.size() + element.placeholder.size() + element.font.size() +
        element.accessible_role.size() + element.accessible_name.size() + element.accessible_description.size() +
        element.mask.size() + element.scroll_target.size() +
        (element.image ? element.image->icon.size() + element.image->path.size() + element.image->theme.size() : 0);
}

bool same_text_layout(const Element& a, const Element& b) {
    return a.font == b.font && a.text == b.text && a.placeholder == b.placeholder &&
        a.password == b.password && a.mask == b.mask && a.preedit == b.preedit &&
        a.wrap == b.wrap && a.ellipsize == b.ellipsize && a.align == b.align &&
        (a.preedit.empty() || a.cursor == b.cursor);
}

void replace_elements(Surface& surface, ParsedTree& parsed, int begin, int end, bool preserve) {
    auto& gui = *surface.gui;
    auto& old = surface.elements;
    const int delta = static_cast<int>(parsed.elements.size()) - (end - begin);
    if (static_cast<int>(old.size()) + delta > 2048) throw std::runtime_error("tree exceeds 2048 elements");
    std::vector<int> remap(old.size(), -1);
    std::size_t bytes = parsed.bytes;
    for (int i = 0; i < static_cast<int>(old.size()); ++i) {
        if (i >= begin && i < end) continue;
        remap[static_cast<std::size_t>(i)] = i < begin ? i : i + delta;
        bytes += element_text_bytes(old[static_cast<std::size_t>(i)]);
    }
    if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    if (preserve) {
        for (std::size_t i = 0; i < parsed.elements.size(); ++i) {
            auto& next = parsed.elements[i];
            if (next.id.empty()) continue;
            const auto found = std::ranges::lower_bound(surface.named_elements, next.id, {},
                [&](int index) -> const std::string& { return old[static_cast<std::size_t>(index)].id; });
            if (found == surface.named_elements.end() || *found < begin || *found >= end) continue;
            const auto& previous = old[static_cast<std::size_t>(*found)];
            if (previous.id != next.id || previous.kind != next.kind) continue;
            remap[static_cast<std::size_t>(*found)] = begin + static_cast<int>(i);
            if (!(parsed.specified[i] & text_specified) && !previous.text.empty()) {
                if (next.font.empty()) throw std::runtime_error("text requires an explicit font");
                bytes += previous.text.size();
                if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
                next.text = previous.text;
            }
            if (!(parsed.specified[i] & value_specified)) next.value = previous.value;
            if (!(parsed.specified[i] & scroll_specified)) {
                next.scroll = previous.scroll;
                next.scroll_origin = previous.scroll_origin;
            }
            const bool same_editor = next.text == previous.text && next.password == previous.password;
            next.edit_revision = previous.edit_revision + (same_editor ? 0 : 1);
            if (next.text == previous.text) {
                next.cursor = previous.cursor; next.anchor = previous.anchor;
            } else next.cursor = next.anchor = static_cast<int>(next.text.size());
            if (same_editor && next.enabled) {
                next.preedit = previous.preedit; next.preedit_seat = previous.preedit_seat;
                next.preedit_cursor = previous.preedit_cursor; next.preedit_end = previous.preedit_end;
            }
        }
    }
    const int count = static_cast<int>(old.size()) + delta;
    const auto next_element = [&](int index) -> const Element& {
        if (index < begin) return old[static_cast<std::size_t>(index)];
        if (index < end + delta) return parsed.elements[static_cast<std::size_t>(index - begin)];
        return old[static_cast<std::size_t>(index - delta)];
    };
    const auto next_parent = [&](int index) {
        int parent = next_element(index).parent;
        if (index >= begin && index < end + delta) return parent < 0 ? begin - 1 : parent + begin;
        return parent >= end ? parent + delta : parent;
    };
    std::vector<int> named, images, scrolls, ranges;
    std::vector<std::array<int, 2>> links(static_cast<std::size_t>(count), {-1, -1});
    named.reserve(std::min(static_cast<std::size_t>(count), surface.named_elements.size()));
    images.reserve(std::min(static_cast<std::size_t>(count), surface.image_elements.size()));
    scrolls.reserve(std::min(static_cast<std::size_t>(count), surface.scroll_elements.size()));
    for (int i = 0; i < count; ++i) {
        const auto& element = next_element(i);
        if (!element.id.empty()) named.push_back(i);
        if (element.kind == Kind::image) images.push_back(i);
        if (element.kind == Kind::scroll) scrolls.push_back(i);
        if (element.kind == Kind::slider) ranges.push_back(i);
        if (element.kind == Kind::scroll || element.kind == Kind::button || element.kind == Kind::toggle ||
            element.kind == Kind::slider || element.kind == Kind::input)
            for (int parent = next_parent(i); parent >= 0; parent = next_parent(parent))
                if (next_element(parent).kind == Kind::slider)
                    throw std::runtime_error("slider children must be drawing or layout elements");
        if (element.thumb) {
            const int parent = next_parent(i);
            if (parent < 0 || next_element(parent).kind != Kind::slider)
                throw std::runtime_error("thumb belongs to a slider");
            auto& thumb = links[static_cast<std::size_t>(parent)][0];
            if (thumb >= 0) throw std::runtime_error("slider accepts one thumb");
            thumb = i;
        }
    }
    std::ranges::sort(named, {}, [&](int index) -> const std::string& { return next_element(index).id; });
    for (std::size_t i = 1; i < named.size(); ++i)
        if (next_element(named[i - 1]).id == next_element(named[i]).id)
            throw std::runtime_error("duplicate element id: " + next_element(named[i]).id);
    for (const int index : ranges) {
        const auto& target = next_element(index).scroll_target;
        if (target.empty()) continue;
        const auto found = std::ranges::lower_bound(named, target, {},
            [&](int i) -> const std::string& { return next_element(i).id; });
        if (found == named.end() || next_element(*found).id != target || next_element(*found).kind != Kind::scroll)
            throw std::runtime_error("scroll_target must name a scroll element");
        for (int parent = next_parent(index); parent >= 0; parent = next_parent(parent))
            if (parent == *found) throw std::runtime_error("scroll_target must be in a separate tree branch");
        links[static_cast<std::size_t>(index)][1] = *found;
    }
    const bool whole_tree = begin == 0 && end == static_cast<int>(old.size());
    std::vector<Element> combined;
    if (!whole_tree) combined.reserve(static_cast<std::size_t>(count));
    const int previous_focus = surface.focused;
    int next_focus = previous_focus < 0 ? -1 : remap[static_cast<std::size_t>(previous_focus)];
    if (next_focus >= 0 && !focusable(next_element(next_focus))) next_focus = -1;
    const bool same_editor = next_focus >= 0 &&
        next_element(next_focus).edit_revision == old[static_cast<std::size_t>(previous_focus)].edit_revision;
    const auto remap_drag = [&](int index) {
        if (index < 0 || old[static_cast<std::size_t>(index)].kind != Kind::slider) return -1;
        const int mapped = remap[static_cast<std::size_t>(index)];
        if (mapped < 0) return -1;
        const auto& next = next_element(mapped);
        const auto& previous = old[static_cast<std::size_t>(index)];
        if ((previous.thumb_index >= 0) != (links[static_cast<std::size_t>(mapped)][0] >= 0)) return -1;
        if (previous.target_index >= 0) {
            const int target = links[static_cast<std::size_t>(mapped)][1];
            if (target < 0 || remap[static_cast<std::size_t>(previous.target_index)] != target ||
                !next_element(target).enabled ||
                next_element(target).horizontal != old[static_cast<std::size_t>(previous.target_index)].horizontal)
                return -1;
        }
        return next.enabled &&
            next.horizontal == previous.horizontal && next.scroll_target == previous.scroll_target ? mapped : -1;
    };
    const int next_drag = remap_drag(surface.pressed);

    // Validation and destination allocation precede this ownership transfer.
    if (next_focus < 0) focus(surface, -1);
    else if (!same_editor) {
        for (auto& seat : gui.seats)
            if (seat->keyboard_surface == surface.id && seat->compose) xkb_compose_state_reset(seat->compose);
    }
    for (int i = begin; i < end; ++i) {
        const int mapped = remap[static_cast<std::size_t>(i)];
        if (mapped < 0) continue;
        auto& previous = old[static_cast<std::size_t>(i)];
        auto& next = parsed.elements[static_cast<std::size_t>(mapped - begin)];
        if (previous.layout && next.font == previous.font) {
            next.layout = std::exchange(previous.layout, nullptr);
            if (same_text_layout(next, previous)) {
                next.text_dirty = previous.text_dirty;
                next.text_width = previous.text_width;
                next.natural_text_width = previous.natural_text_width;
                next.natural_text_height = previous.natural_text_height;
            }
        }
    }
    if (whole_tree) {
        clear_elements(parsed.lua, old);
        old = std::move(parsed.elements);
    } else {
        for (int i = 0; i < begin; ++i) {
            auto& element = old[static_cast<std::size_t>(i)];
            if (element.end >= end) element.end += delta;
            combined.push_back(std::move(element));
        }
        for (auto& element : parsed.elements) {
            element.parent = element.parent < 0 ? begin - 1 : element.parent + begin;
            element.end += begin;
            combined.push_back(std::move(element));
        }
        for (int i = begin; i < end; ++i) {
            auto& element = old[static_cast<std::size_t>(i)];
            if (element.layout) g_object_unref(element.layout);
            unref(parsed.lua, element.callback);
        }
        for (int i = end; i < static_cast<int>(old.size()); ++i) {
            auto& element = old[static_cast<std::size_t>(i)];
            if (element.parent >= end) element.parent += delta;
            element.end += delta;
            combined.push_back(std::move(element));
        }
        parsed.elements.clear();
        old = std::move(combined);
    }
    surface.named_elements = std::move(named);
    surface.image_elements = std::move(images);
    surface.scroll_elements = std::move(scrolls);
    surface.range_elements = std::move(ranges);
    for (const int index : surface.range_elements) {
        auto& element = old[static_cast<std::size_t>(index)];
        element.thumb_index = links[static_cast<std::size_t>(index)][0];
        element.target_index = links[static_cast<std::size_t>(index)][1];
    }
    ++surface.revision; surface.pressed = next_drag;
    surface.key_pressed = -1; surface.content_bytes = bytes;
    surface.focused = next_focus;
    const bool activation_target = next_focus >= 0 &&
        (surface.elements[static_cast<std::size_t>(next_focus)].kind == Kind::button ||
         surface.elements[static_cast<std::size_t>(next_focus)].kind == Kind::toggle);
    for (std::size_t i = 0; i < gui.event_count; ++i) {
        auto& event = gui.events[i];
        if (event.surface != surface.id) continue;
        const bool drag_release = next_drag >= 0 && !event.pressed && event.key == BTN_LEFT &&
            event.seat == surface.press_seat && event.touch == surface.press_touch;
        if ((event.type == Input::Type::button && !drag_release) || (event.type == Input::Type::key &&
            (next_focus < 0 || (activation_target && (event.key == XKB_KEY_space || event.key == XKB_KEY_Return)))))
            event.surface = 0;
        else if (same_editor && (event.type == Input::Type::ime || event.type == Input::Type::ime_leave) &&
                 event.revision == surface.revision - 1 && event.element == previous_focus) {
            event.revision = surface.revision; event.element = next_focus;
        }
    }
    if (same_editor) {
        for (auto& seat : gui.seats) {
            if (seat->ime_surface != surface.id || seat->ime_revision != surface.revision - 1 || seat->ime_element != previous_focus) continue;
            seat->ime_revision = surface.revision; seat->ime_element = next_focus;
        }
        for (auto& transfer : gui.transfers) {
            if (transfer.surface != surface.id || transfer.revision != surface.revision - 1 || transfer.element != previous_focus) continue;
            transfer.revision = surface.revision; transfer.element = next_focus;
        }
    }
    dirty(surface); wake(gui.owner);
}

int set_content(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, surface_type);
    const auto previous_size = live_surface(gui, identity).elements.size();
    luaL_checktype(lua, 2, LUA_TTABLE);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TTABLE);
    const bool preserve = !lua_isnoneornil(lua, 3) && boolean_field(lua, 3, "preserve_state", false);
    ParsedTree parsed{lua, {}, {}};
    parsed.elements.reserve(previous_size); parsed.specified.reserve(previous_size);
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) { parse_tree(lua, 2, parsed, gui, -1, 1); return 0; }, 0, 0, 1) < 0) return -1;
    auto& surface = live_surface(gui, identity);
    replace_elements(surface, parsed, 0, static_cast<int>(surface.elements.size()), preserve);
    lua_pushboolean(lua, true); return 1;
}

int find_element(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, surface_type);
    std::size_t length; const char* id = check_string(lua, 2, &length);
    const auto& surface = live_surface(gui, identity);
    const std::string_view name(id, length);
    const auto found = std::ranges::lower_bound(surface.named_elements, name, {},
        [&](int index) -> const std::string& { return surface.elements[static_cast<std::size_t>(index)].id; });
    if (found != surface.named_elements.end() && surface.elements[static_cast<std::size_t>(*found)].id == name) {
        push_handle(lua, {surface.id, surface.revision, *found}, element_type); return 1;
    }
    lua_pushnil(lua); lua_pushliteral(lua, "element id not found"); return 2;
}

int replace_children(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, element_type);
    const auto& initial = live_surface(gui, identity);
    const auto kind = initial.elements[static_cast<std::size_t>(identity.element)].kind;
    const auto previous_size = static_cast<std::size_t>(initial.elements[static_cast<std::size_t>(identity.element)].end - identity.element - 1);
    if (!container(kind)) throw std::runtime_error("only containers accept children");
    unsigned depth = 2;
    for (int parent = initial.elements[static_cast<std::size_t>(identity.element)].parent; parent >= 0;
        parent = initial.elements[static_cast<std::size_t>(parent)].parent) ++depth;
    luaL_checktype(lua, 2, LUA_TTABLE);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TTABLE);
    const bool preserve = !lua_isnoneornil(lua, 3) && boolean_field(lua, 3, "preserve_state", false);
    ParsedTree parsed{lua, {}, {}};
    parsed.elements.reserve(previous_size); parsed.specified.reserve(previous_size);
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
    replace_elements(surface, parsed, begin, end, preserve);
    lua_pushboolean(lua, true); return 1;
}

int set_element(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, element_type);
    const auto kind = live_surface(gui, identity).elements[static_cast<std::size_t>(identity.element)].kind;
    luaL_checktype(lua, 2, LUA_TTABLE);
    ElementPatch patch(lua);
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        read_properties(lua, 2, patch, gui, kind, false, max_content); return 0;
    }, 0, 0, 1) < 0) return -1;
    auto& live = live_surface(gui, identity);
    auto& element = live.elements[static_cast<std::size_t>(identity.element)];
    const bool horizontal = element.horizontal;
    const auto damage = apply_properties(element, patch, live.content_bytes);
    if (!element.enabled && live.focused == identity.element) focus(live, -1);
    if (live.pressed >= 0 && (!element.enabled || horizontal != element.horizontal)) {
        const auto& pressed = live.elements[static_cast<std::size_t>(live.pressed)];
        if (live.pressed == identity.element || pressed.target_index == identity.element) live.pressed = -1;
    }
    invalidate_element(live, identity.element, damage);
    if (element.image && !element.image->submitted) wake(gui.owner);
    lua_pushboolean(lua, true); return 1;
}

int element_state(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, element_type);
    auto& surface = live_surface(gui, identity); layout(surface);
    const auto& element = surface.elements[static_cast<std::size_t>(identity.element)];
    const auto text = element.text; const auto box = element.box; const auto value = element.value;
    const auto scroll = element.scroll, limit = element.scroll_limit;
    const bool pressed = surface.pressed == identity.element, focused = surface.focused == identity.element;
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 10); string_field(lua, "text", text);
        lua_pushnumber(lua, value); lua_setfield(lua, -2, "value");
        lua_pushnumber(lua, box.x); lua_setfield(lua, -2, "x");
        lua_pushnumber(lua, box.y); lua_setfield(lua, -2, "y");
        lua_pushnumber(lua, box.w); lua_setfield(lua, -2, "width");
        lua_pushnumber(lua, box.h); lua_setfield(lua, -2, "height");
        lua_pushnumber(lua, scroll); lua_setfield(lua, -2, "scroll");
        lua_pushnumber(lua, limit); lua_setfield(lua, -2, "scroll_limit");
        lua_pushboolean(lua, pressed); lua_setfield(lua, -2, "pressed");
        lua_pushboolean(lua, focused); lua_setfield(lua, -2, "focused");
        return 1;
    }, 1);
}

int element_accessibility(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, element_type);
    const auto& surface = live_surface(gui, identity);
    const auto& element = surface.elements[static_cast<std::size_t>(identity.element)];
    static const char* roles[] = {"group", "group", "group", "group", "label", "image", "button", "checkbox", "slider", "textbox"};
    const std::string role = element.accessible_role == "auto" ? roles[static_cast<unsigned>(element.kind)] : element.accessible_role;
    const auto name = element.accessible_name;
    const auto description = element.accessible_description;
    bool hidden = element.accessible_hidden;
    for (int parent = element.parent; parent >= 0; parent = surface.elements[static_cast<std::size_t>(parent)].parent)
        hidden |= surface.elements[static_cast<std::size_t>(parent)].accessible_hidden;
    const bool enabled = element.enabled, can_focus = focusable(element), focused = surface.focused == identity.element;
    const auto kind = element.kind;
    const auto value = element.value;
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 9);
        if (!role.empty()) string_field(lua, "role", role);
        if (!name.empty()) string_field(lua, "name", name);
        if (!description.empty()) string_field(lua, "description", description);
        lua_pushboolean(lua, hidden); lua_setfield(lua, -2, "hidden");
        lua_pushboolean(lua, enabled); lua_setfield(lua, -2, "enabled");
        lua_pushboolean(lua, can_focus); lua_setfield(lua, -2, "focusable");
        lua_pushboolean(lua, focused); lua_setfield(lua, -2, "focused");
        if (kind == Kind::toggle) { lua_pushboolean(lua, value != 0); lua_setfield(lua, -2, "checked"); }
        if (kind == Kind::slider) { lua_pushnumber(lua, value); lua_setfield(lua, -2, "value"); }
        return 1;
    }, 1);
}

int focus_element(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, element_type);
    auto& surface = live_surface(gui, identity);
    if (!focusable(surface.elements[static_cast<std::size_t>(identity.element)])) throw std::runtime_error("element is not focusable");
    focus(surface, identity.element);
    reveal_focus(surface, false);
    lua_pushboolean(lua, true); return 1;
}

const hyprland_focus_grab_v1_listener grab_listener{
    .cleared = [](void* data, hyprland_focus_grab_v1*) {
        auto& surface = *static_cast<Surface*>(data);
        hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
        surface.grab_pending = true;
        cancel_surface_input(surface, true);
    }
};

enum class SurfaceRole { window, layer, popup };

template<SurfaceRole role>
int create_surface(lua_State* lua) {
    auto& gui = gui_state(lua);
    luaL_checktype(lua, 1, LUA_TTABLE);
    auto surface = std::make_unique<Surface>(gui);
    std::string title, app_id, output;
    int width = 0, height = 0, x = 0, y = 0;
    int edge = 0, layer = 0, keyboard = 0, exclusive_zone = 0, margin = 0;
    bool exclusive = false, grab = false;
    lua_pushvalue(lua, 1);
    if (protected_call(lua, [&](lua_State* lua) {
        if constexpr (role != SurfaceRole::layer) {
            width = static_cast<int>(integer_field(lua, 2, "width", std::nullopt, 1));
            height = static_cast<int>(integer_field(lua, 2, "height", std::nullopt, 1));
        }
        if constexpr (role == SurfaceRole::window) text_field(lua, 2, "title", title, 256);
        if constexpr (role != SurfaceRole::popup) text_field(lua, 2, "app_id", app_id, 256);
        if constexpr (role == SurfaceRole::layer) text_field(lua, 2, "output", output, 256);
        callback_field(lua, 2, "on_configure", surface->on_configure);
        callback_field(lua, 2, "on_close", surface->on_close);
        callback_field(lua, 2, "on_grab_cleared", surface->on_grab_cleared);
        callback_field(lua, 2, "on_input", surface->on_input);
        callback_field(lua, 2, "on_focus_change", surface->on_focus_change);
        surface->dismiss_focus_loss = boolean_field(lua, 2, "dismiss_on_focus_loss", false);
        surface->dismiss_escape = boolean_field(lua, 2, "dismiss_on_escape", false);
        surface->dismiss_grab_cleared = boolean_field(lua, 2, "dismiss_on_grab_cleared", false);
        if constexpr (role == SurfaceRole::layer) {
            static const char* edges[] = {"top", "bottom", "left", "right", "all", nullptr};
            static const char* layers[] = {"background", "bottom", "top", "overlay", nullptr};
            static const char* keyboards[] = {"none", "exclusive", "on_demand", nullptr};
            edge = enum_field(lua, 2, "edge", nullptr, edges);
            layer = enum_field(lua, 2, "layer", nullptr, layers);
            keyboard = enum_field(lua, 2, "keyboard", nullptr, keyboards);
            exclusive = boolean_field(lua, 2, "exclusive", false);
            exclusive_zone = static_cast<int>(integer_field(lua, 2, "exclusive_zone", 0, -1));
            margin = static_cast<int>(integer_field(lua, 2, "margin", 0, 0, 4096));
            const bool vertical = edge == 2 || edge == 3;
            width = static_cast<int>(integer_field(lua, 2, "width", std::nullopt, vertical ? 1 : 0));
            height = static_cast<int>(integer_field(lua, 2, "height", std::nullopt, edge < 2 ? 1 : 0));
            if (edge == 4 && (exclusive || exclusive_zone > 0)) luaL_error(lua, "an all-edge layer cannot reserve space");
        }
        if constexpr (role == SurfaceRole::popup) {
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
    if constexpr (role == SurfaceRole::layer) if (!gui.layers) throw std::runtime_error("layer-shell v4 is unavailable");
    if constexpr (role == SurfaceRole::popup) {
        parent = find_surface(gui, surface->parent);
        if (!parent || !parent->configured) throw std::runtime_error("popup parent must be configured");
        if (parent->close_pending) throw std::runtime_error("popup parent is closing");
        for (const auto& item : gui.surfaces)
            if (item && item->parent == parent->id) throw std::runtime_error("parent already has an active or retiring popup");
        if (grab && (!gui.event_seat || !gui.event_serial || gui.event_surface != parent->id))
            throw std::runtime_error("popup grab requires a parent input callback");
        if (grab && parent->popup && parent->popup_grab_seat != gui.event_seat->id)
            throw std::runtime_error("nested popup grab requires a parent popup grabbed by the same seat");
        if (grab && root_surface(*parent).grab)
            throw std::runtime_error("popup cannot take an XDG grab while its root owns a focus grab");
        if (grab) {
            surface->popup_grab_seat = gui.event_seat->id;
            surface->popup_grab_serial = gui.event_serial;
        }
    }
    wl_output* native_output = nullptr;
    if (!output.empty()) {
        for (const auto& item : gui.outputs) if (item->ready && item->state.name == output) {
            native_output = item->native;
            surface->output = item->id;
        }
        if (!native_output) throw std::runtime_error("output not found: " + output);
    }
    surface->pending_width = surface->width = width;
    surface->pending_height = surface->height = height;
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
    if constexpr (role == SurfaceRole::layer) {
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
        if constexpr (role == SurfaceRole::window) {
            surface->toplevel = xdg_surface_get_toplevel(surface->xdg);
            if (!surface->toplevel) throw std::runtime_error("cannot create toplevel");
            xdg_toplevel_add_listener(surface->toplevel, &toplevel_listener, surface.get());
            xdg_toplevel_set_title(surface->toplevel, title.c_str()); xdg_toplevel_set_app_id(surface->toplevel, app_id.c_str());
        } else {
            surface->popup_placement = {x, y, width, height};
        }
    }
    const auto id = surface->id;
    if constexpr (role == SurfaceRole::popup) initialize_popup(*surface);
    else wl_surface_commit(surface->native);
    *slot = std::move(surface);
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
    auto* surface = find_surface(gui, identity.surface);
    const int width = surface ? surface->width : 0, height = surface ? surface->height : 0;
    const double scale = surface ? surface->scale : 1; const bool configured = surface && surface->configured;
    const bool positioned = surface && surface->popup && configured;
    const int x = positioned ? surface->popup_x : 0, y = positioned ? surface->popup_y : 0;
    const auto focus = surface ? surface_focus(*surface) : SurfaceFocus{};
    const bool closing = surface && surface->close_pending;
    const bool mapped = surface && surface->mapped;
    const bool grabbed = surface && root_surface(*surface).grab;
    const auto entered_outputs = surface ? surface->outputs : std::vector<std::uint32_t>{};
    const auto output = surface ? surface->output : 0;
    const char* reason = surface ? surface->close_reason : nullptr;
    const Handle focused{identity.surface, surface ? surface->revision : 0, surface ? surface->focused : -1};
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, positioned ? 16 : 14);
        lua_pushboolean(lua, !surface); lua_setfield(lua, -2, "closed");
        lua_pushboolean(lua, configured); lua_setfield(lua, -2, "configured");
        lua_pushinteger(lua, width); lua_setfield(lua, -2, "width");
        lua_pushinteger(lua, height); lua_setfield(lua, -2, "height");
        lua_pushnumber(lua, scale); lua_setfield(lua, -2, "scale");
        lua_pushboolean(lua, focus.focused); lua_setfield(lua, -2, "focused");
        lua_pushboolean(lua, focus.within); lua_setfield(lua, -2, "focus_within");
        lua_pushboolean(lua, closing); lua_setfield(lua, -2, "closing");
        lua_pushboolean(lua, mapped); lua_setfield(lua, -2, "mapped");
        lua_pushboolean(lua, grabbed); lua_setfield(lua, -2, "focus_grab");
        if (focused.element >= 0) { push_handle(lua, focused, element_type); lua_setfield(lua, -2, "focused_element"); }
        if (reason) { lua_pushstring(lua, reason); lua_setfield(lua, -2, "close_reason"); }
        if (output) { lua_pushinteger(lua, output); lua_setfield(lua, -2, "output_id"); }
        lua_createtable(lua, static_cast<int>(entered_outputs.size()), 0);
        for (std::size_t i = 0; i < entered_outputs.size(); ++i) {
            lua_pushinteger(lua, entered_outputs[i]); lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(lua, -2, "outputs");
        if (positioned) {
            lua_pushinteger(lua, x); lua_setfield(lua, -2, "x");
            lua_pushinteger(lua, y); lua_setfield(lua, -2, "y");
        }
        return 1;
    }, 1);
}

int popup_reposition(lua_State* lua) {
    auto& gui = gui_state(lua);
    const auto identity = handle(lua, 1, surface_type);
    const auto& initial = live_surface(gui, identity);
    if (!initial.parent) throw std::runtime_error("only popups can be repositioned");
    luaL_checktype(lua, 2, LUA_TTABLE);
    auto placement = initial.popup_placement;
    placement.x = static_cast<int>(integer_field(lua, 2, "x", placement.x, -16384));
    placement.y = static_cast<int>(integer_field(lua, 2, "y", placement.y, -16384));
    placement.width = static_cast<int>(integer_field(lua, 2, "width", placement.width, 1));
    placement.height = static_cast<int>(integer_field(lua, 2, "height", placement.height, 1));
    auto& surface = live_surface(gui, identity);
    if (surface.close_pending) throw std::runtime_error("popup is closing");
    surface.popup_placement = placement;
    surface.popup_reposition_pending = true;
    lua_pushboolean(lua, true); return 1;
}

int grab_focus(lua_State* lua) {
    auto& gui = gui_state(lua); const auto identity = handle(lua, 1, surface_type);
    auto& surface = live_surface(gui, identity);
    if (surface.parent) throw std::runtime_error("focus grabs belong to the root surface");
    if (surface.close_pending) throw std::runtime_error("surface is closing");
    if (!gui.focus_grabs || !surface.configured) throw std::runtime_error("focus grab requires a configured surface and compositor support");
    if (!surface.grab) {
        for (const auto& member : gui.surfaces)
            if (member && member->parent && member->popup_grab_seat &&
                &root_surface(*member) == &surface)
                throw std::runtime_error("focus grab conflicts with an active or retiring XDG popup grab");
        surface.grab = hyprland_focus_grab_manager_v1_create_grab(gui.focus_grabs);
        if (!surface.grab) throw std::runtime_error("cannot create focus grab");
        hyprland_focus_grab_v1_add_listener(surface.grab, &grab_listener, &surface);
        for (const auto& previous : gui.surfaces) {
            if (!previous || previous.get() == &surface || !previous->grab) continue;
            hyprland_focus_grab_v1_destroy(std::exchange(previous->grab, nullptr));
            previous->grab_pending = true;
            cancel_surface_input(*previous, true);
        }
        for (const auto& member : gui.surfaces)
            if (member && !member->closed && !member->close_pending && member->configured &&
                &root_surface(*member) == &surface)
                hyprland_focus_grab_v1_add_surface(surface.grab, member->native);
        hyprland_focus_grab_v1_commit(surface.grab);
        ++surface.grab_generation;
        surface.grab_pending = false;
    }
    lua_pushboolean(lua, true); return 1;
}

int release_focus(lua_State* lua) {
    auto& gui = gui_state(lua); auto& surface = live_surface(gui, handle(lua, 1, surface_type));
    if (surface.parent) throw std::runtime_error("focus grabs belong to the root surface");
    if (surface.grab || surface.grab_pending) cancel_surface_input(surface, true);
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    ++surface.grab_generation;
    surface.grab_pending = false;
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

}

void initialize_popup(Surface& surface) {
    if (!surface.parent || surface.popup || surface.closed || surface.close_pending) return;
    auto& gui = *surface.gui;
    auto* parent = find_surface(gui, surface.parent);
    // Hyprland announces nested popup roles only after the parent's mapping commit.
    if (!parent || parent->close_pending || !parent->mapped) return;
    auto* position = popup_positioner(*parent, surface.popup_placement);
    surface.popup = xdg_surface_get_popup(surface.xdg, parent->xdg, position);
    xdg_positioner_destroy(position);
    if (!surface.popup) throw std::runtime_error("cannot create popup");
    xdg_popup_add_listener(surface.popup, &popup_listener, &surface);
    if (parent->layer) zwlr_layer_surface_v1_get_popup(parent->layer, surface.popup);
    if (surface.popup_grab_seat) {
        const auto seat = std::ranges::find_if(gui.seats, [&](const auto& item) { return item->id == surface.popup_grab_seat; });
        if (seat == gui.seats.end()) { request_surface_close(surface, "seat_removed"); return; }
        xdg_popup_grab(surface.popup, (*seat)->native, surface.popup_grab_serial);
    }
    surface.popup_reposition_pending = false;
    wl_surface_commit(surface.native);
}

int notify_surface(Surface& surface, const int& callback, std::optional<std::uint64_t> grab_generation) {
    auto& state = surface.gui->owner;
    if (callback < 0 || surface.closed || state.requests.load(std::memory_order_relaxed)) return 0;
    const int reference = callback;
    lua_pushcfunction(state.lua, traceback);
    lua_rawgeti(state.lua, LUA_REGISTRYINDEX, reference);
    const int result = protected_call(state.lua, [&](lua_State* lua) {
        push_handle(lua, {surface.id, 0, -1}, surface_type);
        if (surface.closed || state.requests.load(std::memory_order_relaxed) ||
            !callback_matches(lua, callback, reference, 1)) return 0;
        if (grab_generation && *grab_generation != surface.grab_generation) return 0;
        lua_call(lua, 1, 0); return 0;
    }, 0, 1, 1);
    if (result >= 0) lua_settop(state.lua, 0);
    return result;
}

int notify_element(Surface& surface, int index, const char* kind, std::shared_ptr<Image> loaded) {
    auto& gui = *surface.gui;
    const auto& element = surface.elements[static_cast<std::size_t>(index)];
    const int reference = element.callback;
    if (reference < 0 || gui.owner.requests.load(std::memory_order_relaxed)) return 0;
    const auto revision = surface.revision;
    const auto text = element.text;
    const double value = element.value, scroll = element.scroll;
    const bool scrolling = element.kind == Kind::scroll;
    lua_pushcfunction(gui.owner.lua, traceback);
    lua_rawgeti(gui.owner.lua, LUA_REGISTRYINDEX, reference);
    const int result = protected_call(gui.owner.lua, [&](lua_State* lua) {
        push_handle(lua, {surface.id, revision, index}, element_type);
        lua_createtable(lua, 0, 5);
        lua_pushstring(lua, kind); lua_setfield(lua, -2, "kind");
        string_field(lua, "text", text); lua_pushnumber(lua, value); lua_setfield(lua, -2, "value");
        if (gui.event_seat) { lua_pushinteger(lua, gui.event_seat->id); lua_setfield(lua, -2, "seat"); }
        if (scrolling) { lua_pushnumber(lua, scroll); lua_setfield(lua, -2, "scroll"); }
        if (loaded && !loaded->error.empty()) string_field(lua, "error", loaded->error);
        if (surface.closed || surface.revision != revision || gui.owner.requests.load(std::memory_order_relaxed)) return 0;
        const auto& live = surface.elements[static_cast<std::size_t>(index)];
        if ((loaded && live.image != loaded) || !callback_matches(lua, live.callback, reference, 2)) return 0;
        lua_call(lua, 2, 0); return 0;
    }, 0, 1, 1);
    if (result >= 0) lua_settop(gui.owner.lua, 0);
    return result;
}

void register_surfaces(lua_State* lua) {
    luaL_newmetatable(lua, surface_type);
    const luaL_Reg surfaces[] = {
        {"close", operation_call<surface_close>}, {"state", operation_call<surface_state>},
        {"set_content", operation_call<set_content>}, {"find", operation_call<find_element>},
        {"reposition", operation_call<popup_reposition>},
        {"grab_focus", operation_call<grab_focus>}, {"release_focus", operation_call<release_focus>},
        {"set_input_region", operation_call<input_region>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, surfaces, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, operation_call<surface_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks surface"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    luaL_newmetatable(lua, element_type);
    const luaL_Reg elements[] = {
        {"set", operation_call<set_element>}, {"state", operation_call<element_state>}, {"focus", operation_call<focus_element>},
        {"accessibility", operation_call<element_accessibility>},
        {"replace_children", operation_call<replace_children>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, elements, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushliteral(lua, "hyprworks element"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    const luaL_Reg functions[] = {
        {"layer", operation_call<create_surface<SurfaceRole::layer>>}, {"window", operation_call<create_surface<SurfaceRole::window>>},
        {"popup", operation_call<create_surface<SurfaceRole::popup>>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
