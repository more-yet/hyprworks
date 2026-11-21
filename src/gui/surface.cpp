#include "../runtime/invocation.hpp"
#include "gui.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>

namespace hyprworks::lua {

Surface* find_surface(Gui& gui, std::uint64_t id) {
    for (auto& surface : gui.surfaces)
        if (surface && surface->id == id && !surface->closed) return surface.get();
    return nullptr;
}

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

namespace {

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

void destroy_surface(Surface& surface) {
    auto& gui = *surface.gui;
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    for (auto& child : gui.surfaces)
        if (child && child->parent == surface.id) destroy_surface(*child);
    if (surface.native && surface.parent) {
        auto& root = root_surface(surface);
        if (root.grab) {
            hyprland_focus_grab_v1_remove_surface(root.grab, surface.native);
            hyprland_focus_grab_v1_commit(root.grab);
        }
    }
    surface.parent = 0;
    surface.closed = true;
    if (surface.frame) wl_callback_destroy(std::exchange(surface.frame, nullptr));
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
    surface.pressed = -1;
    if (surface.grab) hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
    surface.grab_pending = false;
    for (auto& seat : surface.gui->seats)
        if (seat->keyboard_surface == surface.id) seat->repeat_at.reset();
    for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id) close_surface(*child);
}

int dispatch_close(Surface& surface) {
    if (surface.closed) return 0;
    auto& gui = *surface.gui;
    for (auto& child : gui.surfaces)
        if (child && child->parent == surface.id && child->close_pending && dispatch_close(*child) < 0) return -1;
    if (notify(gui, surface.id, -1, surface.on_close) < 0) return -1;
    close_surface(surface);
    return 0;
}

void update_scale(Surface& surface) {
    if (surface.fractional) return;
    int scale = 1;
    for (const auto& output : surface.gui->outputs)
        if (std::ranges::find(surface.outputs, output->id) != surface.outputs.end()) scale = std::max(scale, output->scale);
    if (surface.scale != scale) { surface.scale = scale; dirty(surface); surface.configure_pending = true; }
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
    if (value.element >= 0 && (value.revision != surface->revision || static_cast<std::size_t>(value.element) >= surface->elements.size()))
        throw std::runtime_error("element belongs to a replaced tree");
    return *surface;
}

void push_handle(lua_State* lua, Handle value, const char* type) {
    *static_cast<Handle*>(lua_newuserdatauv(lua, sizeof(Handle), 0)) = value;
    luaL_setmetatable(lua, type);
}

namespace {

void request_close(Surface& surface) {
    if (surface.closed) return;
    surface.close_pending = true;
    for (auto& child : surface.gui->surfaces)
        if (child && child->parent == surface.id) request_close(*child);
}

void set_configure(Surface& surface, int width, int height) {
    if (surface.closed || surface.close_pending) return;
    if (width < 1 || height < 1 || width > 16384 || height > 16384) {
        surface.gui->error = "configured surface dimensions outside 1..16384";
        return;
    }
    const bool initial = !surface.configured;
    surface.width = width; surface.height = height;
    surface.configured = true; surface.configure_pending = true;
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
        set_configure(surface, surface.pending_width, surface.pending_height);
    }
};
const xdg_toplevel_listener toplevel_listener{
    .configure = [](void* data, xdg_toplevel*, int width, int height, wl_array*) {
        auto& surface = *static_cast<Surface*>(data);
        if (width > 0) surface.pending_width = width;
        if (height > 0) surface.pending_height = height;
    },
    .close = [](void* data, xdg_toplevel*) { request_close(*static_cast<Surface*>(data)); },
    .configure_bounds = [](void*, xdg_toplevel*, int, int) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};
const xdg_popup_listener popup_listener{
    .configure = [](void* data, xdg_popup*, int, int, int width, int height) {
        auto& surface = *static_cast<Surface*>(data);
        surface.pending_width = width; surface.pending_height = height;
    },
    .popup_done = [](void* data, xdg_popup*) { request_close(*static_cast<Surface*>(data)); },
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
    .closed = [](void* data, zwlr_layer_surface_v1*) { request_close(*static_cast<Surface*>(data)); }
};

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
        field(lua, table, "scroll");
        if (!lua_isnil(lua, -1)) element.scroll_origin.reset();
        lua_pop(lua, 1);
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
        if (auto* image = static_cast<std::shared_ptr<Image>*>(luaL_testudata(lua, -1, captured_image_type))) {
            element.image = *image; element.image_notified = false;
        } else if (!lua_isnil(lua, -1)) {
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
    std::vector<unsigned> specified;
    ~ParsedTree() { clear_elements(lua, elements); }
};

constexpr unsigned text_specified = 1, value_specified = 2, scroll_specified = 4;

int parse_tree(lua_State* lua, int table, ParsedTree& tree, Gui& gui, int parent, unsigned depth) {
    if (!lua_checkstack(lua, 12)) luaL_error(lua, "cannot grow Lua parsing stack");
    if (depth > 32 || tree.elements.size() == 2048) luaL_error(lua, "tree limit reached (2048 elements, depth 32)");
    luaL_checktype(lua, table, LUA_TTABLE);
    table = lua_absindex(lua, table);
    static const char* kinds[] = {"row", "column", "scroll", "box", "text", "image", "button", "toggle", "slider", "input", nullptr};
    const auto kind = static_cast<Kind>(enum_field(lua, table, "kind", "box", kinds));
    const int index = static_cast<int>(tree.elements.size());
    tree.elements.emplace_back();
    auto& element = tree.elements.back(); element.kind = kind; element.parent = parent;
    unsigned specified = 0;
    for (const auto& [name, bit] : {std::pair{"text", text_specified}, {"value", value_specified}, {"scroll", scroll_specified}}) {
        field(lua, table, name);
        if (!lua_isnil(lua, -1)) specified |= bit;
        lua_pop(lua, 1);
    }
    tree.specified.push_back(specified);
    if (kind == Kind::button || kind == Kind::toggle || kind == Kind::input) {
        element.padding = 6; element.radius = 4; element.background = 0x303238ff;
    }
    properties(lua, table, element, gui);
    field(lua, table, "children");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        if (kind != Kind::row && kind != Kind::column && kind != Kind::scroll && kind != Kind::box && kind != Kind::button)
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

void replace_elements(Surface& surface, ParsedTree& parsed, int begin, int end, bool preserve) {
    auto& gui = *surface.gui;
    auto& old = surface.elements;
    const int delta = static_cast<int>(parsed.elements.size()) - (end - begin);
    if (static_cast<int>(old.size()) + delta > 2048) throw std::runtime_error("tree exceeds 2048 elements");
    std::vector<int> remap(old.size(), -1);
    for (int i = 0; i < static_cast<int>(old.size()); ++i)
        if (i < begin || i >= end) remap[static_cast<std::size_t>(i)] = i < begin ? i : i + delta;
    if (preserve) {
        for (std::size_t i = 0; i < parsed.elements.size(); ++i) {
            auto& next = parsed.elements[i];
            if (next.id.empty()) continue;
            for (int j = begin; j < end; ++j) {
                const auto& previous = old[static_cast<std::size_t>(j)];
                if (previous.id != next.id || previous.kind != next.kind) continue;
                remap[static_cast<std::size_t>(j)] = begin + static_cast<int>(i);
                if (!(parsed.specified[i] & text_specified)) next.text = previous.text;
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
                break;
            }
        }
    }
    auto bytes = content_bytes(parsed.elements);
    for (int i = 0; i < static_cast<int>(old.size()); ++i) {
        if (i >= begin && i < end) continue;
        const auto& retained = old[static_cast<std::size_t>(i)];
        bytes += element_text_bytes(retained);
        if (!retained.id.empty()) for (const auto& added : parsed.elements)
            if (retained.id == added.id) throw std::runtime_error("duplicate element id: " + retained.id);
    }
    if (bytes > max_content) throw std::runtime_error("content strings exceed 1 MiB");
    std::vector<Element> combined;
    combined.reserve(static_cast<std::size_t>(static_cast<int>(old.size()) + delta));
    const int previous_focus = surface.focused;
    int next_focus = previous_focus < 0 ? -1 : remap[static_cast<std::size_t>(previous_focus)];
    const Element* next_editor = nullptr;
    if (next_focus >= 0) {
        next_editor = previous_focus >= begin && previous_focus < end ?
            &parsed.elements[static_cast<std::size_t>(next_focus - begin)] : &old[static_cast<std::size_t>(previous_focus)];
        if (!focusable(*next_editor)) next_focus = -1;
    }
    const bool same_editor = next_focus >= 0 &&
        next_editor->edit_revision == old[static_cast<std::size_t>(previous_focus)].edit_revision;

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
        if (next.font == previous.font) next.layout = std::exchange(previous.layout, nullptr);
    }
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
    ++surface.revision; surface.pressed = -1; surface.content_bytes = bytes;
    surface.focused = next_focus;
    for (std::size_t i = 0; i < gui.event_count; ++i) {
        auto& event = gui.events[i];
        if (event.surface != surface.id) continue;
        if (event.type == Input::Type::button || (event.type == Input::Type::key && next_focus < 0))
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
    live_surface(gui, identity);
    luaL_checktype(lua, 2, LUA_TTABLE);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TTABLE);
    const bool preserve = !lua_isnoneornil(lua, 3) && boolean_field(lua, 3, "preserve_state", false);
    ParsedTree parsed{lua, {}, {}};
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) { parse_tree(lua, 2, parsed, gui, -1, 1); return 0; }, 0, 0, 1) < 0) return -1;
    auto& surface = live_surface(gui, identity);
    replace_elements(surface, parsed, 0, static_cast<int>(surface.elements.size()), preserve);
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
    if (kind != Kind::row && kind != Kind::column &&
        kind != Kind::scroll && kind != Kind::box && kind != Kind::button)
        throw std::runtime_error("only containers accept children");
    unsigned depth = 2;
    for (int parent = initial.elements[static_cast<std::size_t>(identity.element)].parent; parent >= 0;
        parent = initial.elements[static_cast<std::size_t>(parent)].parent) ++depth;
    luaL_checktype(lua, 2, LUA_TTABLE);
    if (!lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TTABLE);
    const bool preserve = !lua_isnoneornil(lua, 3) && boolean_field(lua, 3, "preserve_state", false);
    ParsedTree parsed{lua, {}, {}};
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
    auto& surface = live_surface(gui, identity);
    luaL_checktype(lua, 2, LUA_TTABLE);
    ParsedTree parsed{lua, {}, {}};
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
    reveal_focus(surface, false);
    lua_pushboolean(lua, true); return 1;
}

const hyprland_focus_grab_v1_listener grab_listener{
    .cleared = [](void* data, hyprland_focus_grab_v1*) {
        auto& surface = *static_cast<Surface*>(data);
        hyprland_focus_grab_v1_destroy(std::exchange(surface.grab, nullptr));
        surface.grab_pending = true;
    }
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
        if (parent->close_pending) throw std::runtime_error("popup parent is closing");
        for (const auto& item : gui.surfaces)
            if (item && item->parent == parent->id) throw std::runtime_error("parent already has an active or retiring popup");
        if (grab && (!gui.event_seat || !gui.event_serial || gui.event_surface != parent->id))
            throw std::runtime_error("popup grab requires a parent input callback");
        if (grab && parent->popup && parent->popup_grab_seat != gui.event_seat->id)
            throw std::runtime_error("nested popup grab requires a parent popup grabbed by the same seat");
        if (grab && root_surface(*parent).grab)
            throw std::runtime_error("popup cannot take an XDG grab while its root owns a focus grab");
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
            if (grab) {
                xdg_popup_grab(surface->popup, gui.event_seat->native, gui.event_serial);
                surface->popup_grab_seat = gui.event_seat->id;
            }
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
    if (surface.parent) throw std::runtime_error("focus grabs belong to the root surface");
    if (surface.close_pending) throw std::runtime_error("surface is closing");
    if (!gui.focus_grabs || !surface.configured) throw std::runtime_error("focus grab requires a configured surface and compositor support");
    if (!surface.grab) {
        for (const auto& member : gui.surfaces)
            if (member && member->popup && member->popup_grab_seat &&
                &root_surface(*member) == &surface)
                throw std::runtime_error("focus grab conflicts with an active or retiring XDG popup grab");
        surface.grab = hyprland_focus_grab_manager_v1_create_grab(gui.focus_grabs);
        if (!surface.grab) throw std::runtime_error("cannot create focus grab");
        hyprland_focus_grab_v1_add_listener(surface.grab, &grab_listener, &surface);
        for (const auto& previous : gui.surfaces) {
            if (!previous || previous.get() == &surface || !previous->grab) continue;
            hyprland_focus_grab_v1_destroy(std::exchange(previous->grab, nullptr));
            previous->grab_pending = true;
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

int notify(Gui& gui, std::uint64_t id, int element_index, int reference, const char* kind, const std::string* error) {
    if (reference < 0 || gui.owner.quit_requested.load(std::memory_order_relaxed)) return 0;
    auto* surface = id ? find_surface(gui, id) : nullptr;
    if (id && !surface) return 0;
    const auto revision = surface ? surface->revision : 0;
    const bool grab_cleared = surface && reference == surface->on_grab_cleared;
    const auto grab_generation = grab_cleared ? surface->grab_generation : 0;
    std::string text, message = error ? *error : "";
    double value = 0, scroll = 0;
    bool scrolling = false;
    if (surface && element_index >= 0) {
        const auto& element = surface->elements[static_cast<std::size_t>(element_index)];
        text = element.text; value = element.value; scroll = element.scroll;
        scrolling = element.kind == Kind::scroll;
    }
    lua_pushcfunction(gui.owner.lua, traceback);
    const int result = protected_call(gui.owner.lua, [&](lua_State* lua) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
        if (id) push_handle(lua, {id, revision, element_index}, element_index < 0 ? surface_type : element_type);
        if (element_index >= 0) {
            lua_createtable(lua, 0, 4);
            string_field(lua, "text", text); lua_pushnumber(lua, value); lua_setfield(lua, -2, "value");
            if (scrolling) { lua_pushnumber(lua, scroll); lua_setfield(lua, -2, "scroll"); }
            if (kind) { lua_pushstring(lua, kind); lua_setfield(lua, -2, "kind"); }
            if (!message.empty()) string_field(lua, "error", message);
        }
        if (id) {
            auto* live = find_surface(gui, id);
            if (!live || (element_index >= 0 && (live->revision != revision ||
                live->elements[static_cast<std::size_t>(element_index)].callback != reference))) return 0;
            if (grab_cleared && live->grab_generation != grab_generation) return 0;
        }
        lua_call(lua, id ? (element_index >= 0 ? 2 : 1) : 0, 0); return 0;
    }, 0, 1);
    if (result >= 0) lua_settop(gui.owner.lua, 0);
    return result;
}

void register_surfaces(lua_State* lua) {
    luaL_newmetatable(lua, surface_type);
    const luaL_Reg surfaces[] = {
        {"close", operation_call<surface_close>}, {"state", operation_call<surface_state>},
        {"set_content", operation_call<set_content>}, {"find", operation_call<find_element>},
        {"grab_focus", operation_call<grab_focus>}, {"release_focus", operation_call<release_focus>},
        {"set_input_region", operation_call<input_region>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, surfaces, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, operation_call<surface_close>); lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks surface"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    luaL_newmetatable(lua, element_type);
    const luaL_Reg elements[] = {
        {"set", operation_call<set_element>}, {"state", operation_call<element_state>}, {"focus", operation_call<focus_element>},
        {"replace_children", operation_call<replace_children>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, elements, 0); lua_pushvalue(lua, -1); lua_setfield(lua, -2, "__index");
    lua_pushliteral(lua, "hyprworks element"); lua_setfield(lua, -2, "__metatable"); lua_pop(lua, 1);
    const luaL_Reg functions[] = {
        {"layer", operation_call<create_surface<1>>}, {"window", operation_call<create_surface<0>>}, {"popup", operation_call<create_surface<2>>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
