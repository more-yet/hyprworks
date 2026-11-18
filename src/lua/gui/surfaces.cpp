#include "surface.hpp"

#include <hyprtoolkit/element/Element.hpp>
#include <algorithm>
#include <xkbcommon/xkbcommon.h>

namespace hyprworks::lua {
namespace {

constexpr char surface_metatable[] = "hyprworks.surface";
constexpr std::size_t max_surfaces = 32;

}

void close_surface(Surface& surface, bool notify) {
    const bool was_closed = std::exchange(surface.closed, true);
    if (surface.popup)
        close_surface(*surface.popup, notify);
    if (!was_closed) {
        for (const auto& child : surface.owner.surfaces) {
            if (child->parent == &surface)
                close_surface(*child, notify);
        }
    }
    if (surface.parent && surface.parent->popup == &surface)
        surface.parent->popup = nullptr;
    surface.parent = nullptr;
    surface.close_pending = false;
    surface.resize_pending = false;
    surface.visible = false;
    surface.close_listener.reset();
    surface.role_listener.reset();
    surface.resize_listener.reset();
    surface.scale_listener.reset();
    surface.output_listener.reset();
    surface.focus_listener.reset();
    surface.focus_pending = false;
    ++surface.focus_generation;
    if (surface.window) {
        surface.window->m_rootElement->setKeyFilter({});
        surface.window->m_rootElement->clearChildren();
        surface.content.reset();
        surface.window->close();
        surface.window.reset();
    }
    surface.content.reset();
    for (auto& shortcut : surface.shortcuts)
        unref(surface.owner.lua, shortcut.callback);
    surface.shortcuts.clear();
    unref(surface.owner.lua, surface.on_resize);
    unref(surface.owner.lua, surface.on_scale);
    unref(surface.owner.lua, surface.on_focus_cleared);
    if (!notify) {
        surface.notify_close = false;
        unref(surface.owner.lua, surface.on_close);
    } else if (!was_closed && surface.on_close != LUA_NOREF) {
        surface.notify_close = true;
        wake(surface.owner);
    }
}

Surface& surface_handle(lua_State* lua, bool allow_closed) {
    auto& surface = **static_cast<Surface**>(luaL_checkudata(lua, 1, surface_metatable));
    if (surface.owner.closing)
        luaL_error(lua, "invocation is shutting down");
    if (surface.closed && !allow_closed)
        luaL_error(lua, "surface is closed");
    return surface;
}

void hide_popup(Surface& surface) {
    if (surface.popup)
        hide_popup(*surface.popup);
    if (!surface.window)
        return;
    ++surface.epoch;
    surface.close_listener.reset();
    surface.role_listener.reset();
    surface.resize_listener.reset();
    surface.scale_listener.reset();
    surface.window->m_rootElement->setKeyFilter({});
    surface.focus_listener.reset();
    surface.focus_pending = false;
    ++surface.focus_generation;
    surface.window->m_rootElement->clearChildren();
    surface.window->close();
    surface.window.reset();
    if (surface.parent && surface.parent->popup == &surface)
        surface.parent->popup = nullptr;
    surface.visible = false;
    surface.close_pending = false;
    surface.resize_pending = false;
    surface.scale_pending = false;
    surface.notify_close = surface.on_close != LUA_NOREF;
    wake(surface.owner);
}

Surface::~Surface() {
    close_surface(*this, false);
    unref(owner.lua, handle);
}

namespace {

void read_input_region(lua_State* lua, int index, std::optional<Hyprutils::Math::CRegion>& region) {
    if (lua_isnoneornil(lua, index)) {
        region.reset();
        return;
    }
    luaL_checktype(lua, index, LUA_TTABLE);
    index = lua_absindex(lua, index);
    const auto count = lua_rawlen(lua, index);
    if (count > 256)
        luaL_error(lua, "input_region accepts at most 256 rectangles");
    region.emplace();
    for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(lua, index, static_cast<lua_Integer>(i));
        luaL_checktype(lua, -1, LUA_TTABLE);
        const auto x = integer_field(lua, -1, "x", 0, -16384);
        const auto y = integer_field(lua, -1, "y", 0, -16384);
        const auto width = integer_field(lua, -1, "width", 0, 1);
        const auto height = integer_field(lua, -1, "height", 0, 1);
        region->add(static_cast<double>(x), static_cast<double>(y), static_cast<double>(width), static_cast<double>(height));
        lua_pop(lua, 1);
    }
}

struct WindowOptions {
    std::string title = "Hyprworks";
    std::string app_id = "hyprworks";
    std::string output;
    int width = 640, height = 480;
    int x = 0, y = 0;
    std::uint32_t layer = 2, anchors = 0, keyboard = 0;
    int exclusive_zone = 0;
    int top = 0, bottom = 0, left = 0, right = 0;
    bool resizable = true, visible = true;
};

void listen_surface(Surface& surface) {
    auto* entry = &surface;
    const auto request_close = [entry]() noexcept {
        entry->close_pending = true;
        wake(entry->owner);
    };
    surface.close_listener = surface.window->m_events.closeRequest.listen(request_close);
    if (surface.kind == ht::HT_WINDOW_POPUP)
        surface.role_listener = surface.window->m_events.popupClosed.listen(request_close);
    else if (surface.kind == ht::HT_WINDOW_LAYER)
        surface.role_listener = surface.window->m_events.layerClosed.listen(request_close);
    surface.window->m_rootElement->setKeyFilter([entry](const auto& event) { return keyboard_input(*entry, event); });
    surface.resize_listener = surface.window->m_events.resized.listen([entry](const Hyprutils::Math::Vector2D& size) noexcept {
        entry->size = size;
        entry->resize_pending = true;
        wake(entry->owner);
    });
    surface.scale_listener = surface.window->m_events.scaleChanged.listen([entry](float) noexcept {
        entry->scale_pending = true;
        wake(entry->owner);
    });
    surface.focus_listener = surface.window->m_events.focusGrabCleared.listen([entry]() noexcept {
        entry->focus_pending = true;
        wake(entry->owner);
    });
}

Hyprutils::Math::Vector2D popup_position(const Surface& surface, Hyprutils::Math::Vector2D position) {
    auto& parent = *surface.parent;
    if (parent.closed || parent.close_pending || !parent.visible || parent.size.x <= 0 || parent.size.y <= 0)
        throw std::runtime_error("popup parent must be a visible, configured surface");
    if (!surface.anchor.empty()) {
        const Element* anchor = nullptr;
        if (parent.content) {
            for (const auto& element : parent.content->elements) {
                if (element.id == surface.anchor) { anchor = &element; break; }
            }
        }
        if (!anchor)
            throw std::runtime_error("unknown popup anchor id");
        const auto box = anchor->native->geometry();
        if (box.w <= 0 || box.h <= 0 || box.x >= parent.size.x || box.y >= parent.size.y || box.x + box.w <= 0 || box.y + box.h <= 0)
            throw std::runtime_error("popup anchor has no visible layout extent");
        position = {std::clamp(box.x + position.x, 0.0, parent.size.x - 1), std::clamp(box.y + box.h + position.y, 0.0, parent.size.y - 1)};
    }
    if (position.x < 0 || position.y < 0 || position.x >= parent.size.x || position.y >= parent.size.y)
        throw std::runtime_error("popup position must be inside the parent surface");
    return position;
}

void open_popup(Surface& surface) {
    auto& parent = *surface.parent;
    if (parent.popup)
        throw std::runtime_error("parent already has an open popup");
    const auto position = popup_position(surface, surface.popup_position);
    surface.window = ht::CWindowBuilder::begin()->type(ht::HT_WINDOW_POPUP)->parent(parent.window)->pos(position)->preferredSize(surface.popup_size)->commence();
    if (!surface.window)
        throw std::runtime_error("cannot create toolkit popup");
    try {
        listen_surface(surface);
        if (surface.input_region && !surface.window->setInputRegion(surface.input_region))
            throw std::runtime_error("toolkit surface input regions are unavailable");
        if (surface.content)
            surface.window->m_rootElement->addChild(surface.content->elements.front().native);
        surface.visible = true;
        parent.popup = &surface;
        surface.window->open();
    } catch (...) {
        hide_popup(surface);
        throw;
    }
}

template<ht::eWindowType Kind>
int create_surface(lua_State* lua) {
    auto& gui = gui_state(lua);
    luaL_checktype(lua, 1, LUA_TTABLE);
    auto surface = std::make_unique<Surface>(gui);
    surface->kind = Kind;
    WindowOptions options;
    if constexpr (Kind == ht::HT_WINDOW_LAYER) {
        options.width = 0;
        options.height = 32;
    } else if constexpr (Kind == ht::HT_WINDOW_POPUP) {
        options.width = 200;
        options.height = 200;
    }
    lua_pushvalue(lua, 1);
    if (protected_call(lua, [&](lua_State* lua) {
        if constexpr (Kind != ht::HT_WINDOW_POPUP) {
            text_field(lua, 2, "title", options.title);
            text_field(lua, 2, "app_id", options.app_id);
        }
        options.width = static_cast<int>(integer_field(lua, 2, "width", options.width, Kind == ht::HT_WINDOW_LAYER ? 0 : 1));
        options.height = static_cast<int>(integer_field(lua, 2, "height", options.height, Kind == ht::HT_WINDOW_LAYER ? 0 : 1));
        options.visible = boolean_field(lua, 2, "visible", true);
        field(lua, 2, "input_region");
        read_input_region(lua, -1, surface->input_region);
        lua_pop(lua, 1);
        if constexpr (Kind == ht::HT_WINDOW_TOPLEVEL)
            options.resizable = boolean_field(lua, 2, "resizable", true);
        callback_field(lua, 2, "on_close", surface->on_close);
        callback_field(lua, 2, "on_resize", surface->on_resize);
        callback_field(lua, 2, "on_scale", surface->on_scale);
        callback_field(lua, 2, "on_focus_cleared", surface->on_focus_cleared);
        field(lua, 2, "shortcuts");
        if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1);
            if (count > 64)
                return luaL_error(lua, "surface accepts at most 64 shortcuts");
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(i));
                luaL_checktype(lua, -1, LUA_TTABLE);
                surface->shortcuts.emplace_back();
                auto& shortcut = surface->shortcuts.back();
                field(lua, -1, "key");
                shortcut.key = xkb_keysym_to_lower(xkb_keysym_from_name(luaL_checkstring(lua, -1), XKB_KEYSYM_CASE_INSENSITIVE));
                lua_pop(lua, 1);
                if (!shortcut.key)
                    return luaL_error(lua, "unknown shortcut keysym");
                if (boolean_field(lua, -1, "ctrl", false)) shortcut.modifiers |= ht::Input::HT_MODIFIER_CTRL;
                if (boolean_field(lua, -1, "shift", false)) shortcut.modifiers |= ht::Input::HT_MODIFIER_SHIFT;
                if (boolean_field(lua, -1, "alt", false)) shortcut.modifiers |= ht::Input::HT_MODIFIER_ALT;
                if (boolean_field(lua, -1, "super", false)) shortcut.modifiers |= ht::Input::HT_MODIFIER_META;
                callback_field(lua, -1, "on_press", shortcut.callback);
                if (shortcut.callback == LUA_NOREF)
                    return luaL_error(lua, "shortcut requires on_press");
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);
        if constexpr (Kind == ht::HT_WINDOW_POPUP) {
            field(lua, 2, "parent");
            surface->parent = *static_cast<Surface**>(luaL_checkudata(lua, -1, surface_metatable));
            lua_pop(lua, 1);
            options.x = static_cast<int>(integer_field(lua, 2, "x", 0, -16384));
            options.y = static_cast<int>(integer_field(lua, 2, "y", 0, -16384));
            text_field(lua, 2, "anchor", surface->anchor, 128);
        } else if constexpr (Kind == ht::HT_WINDOW_LAYER) {
            static const char* layers[] = {"background", "bottom", "top", "overlay", nullptr};
            static const char* keyboards[] = {"none", "exclusive", "on_demand", nullptr};
            options.layer = static_cast<std::uint32_t>(enum_field(lua, 2, "layer", "top", layers));
            options.keyboard = static_cast<std::uint32_t>(enum_field(lua, 2, "keyboard", "none", keyboards));
            text_field(lua, 2, "output", options.output);
            field(lua, 2, "anchors");
            if (lua_isnil(lua, -1))
                options.anchors = 1 | 4 | 8;
            else {
                luaL_checktype(lua, -1, LUA_TTABLE);
                if (boolean_field(lua, -1, "top", false)) options.anchors |= 1;
                if (boolean_field(lua, -1, "bottom", false)) options.anchors |= 2;
                if (boolean_field(lua, -1, "left", false)) options.anchors |= 4;
                if (boolean_field(lua, -1, "right", false)) options.anchors |= 8;
            }
            lua_pop(lua, 1);
            if ((options.width == 0 && (options.anchors & 12) != 12) ||
                (options.height == 0 && (options.anchors & 3) != 3))
                return luaL_error(lua, "zero layer dimensions require opposing anchors");
            options.exclusive_zone = static_cast<int>(integer_field(lua, 2, "exclusive_zone", 0, -1));
            options.top = static_cast<int>(integer_field(lua, 2, "margin_top", 0, -16384));
            options.bottom = static_cast<int>(integer_field(lua, 2, "margin_bottom", 0, -16384));
            options.left = static_cast<int>(integer_field(lua, 2, "margin_left", 0, -16384));
            options.right = static_cast<int>(integer_field(lua, 2, "margin_right", 0, -16384));
        }
        return 0;
    }, 0, 0, 1) < 0)
        return -1;
    // Parsing can run Lua finalizers that create other surfaces.
    if (gui.surfaces.size() >= max_surfaces)
        throw std::runtime_error("invocation surface handle limit reached");
    if constexpr (Kind == ht::HT_WINDOW_POPUP) {
        const auto& parent = *surface->parent;
        if (parent.closed)
            throw std::runtime_error("popup parent is closed");
    }

    const auto backend = gui_backend(gui);
    auto* entry = surface.get();
    if constexpr (Kind == ht::HT_WINDOW_POPUP) {
        surface->popup_position = {options.x, options.y};
        surface->popup_size = {options.width, options.height};
        if (options.visible)
            open_popup(*surface);
    } else {
        auto builder = ht::CWindowBuilder::begin()->type(Kind)
            ->appTitle(std::move(options.title))->appClass(std::move(options.app_id))
            ->preferredSize({options.width, options.height})->resizable(options.resizable);
        if constexpr (Kind == ht::HT_WINDOW_LAYER) {
            builder->layer(options.layer)->anchor(options.anchors)->exclusiveZone(options.exclusive_zone)
                ->kbInteractive(options.keyboard)->marginTopLeft({options.left, options.top})
                ->marginBottomRight({options.right, options.bottom});
            if (!options.output.empty()) {
                const auto outputs = backend->getOutputs();
                const auto found = std::ranges::find_if(outputs, [&](const auto& output) { return output->port() == options.output; });
                if (found == outputs.end())
                    throw std::runtime_error("output not found: " + options.output);
                builder->prefferedOutput(*found);
                surface->output_listener = (*found)->m_events.removed.listen([entry]() noexcept {
                    entry->close_pending = true;
                    wake(entry->owner);
                });
            }
        }
        surface->window = builder->commence();
        if (!surface->window)
            throw std::runtime_error("cannot create toolkit surface");
        listen_surface(*surface);
        if (surface->input_region && !surface->window->setInputRegion(surface->input_region))
            throw std::runtime_error("toolkit surface input regions are unavailable");
        surface->visible = options.visible;
        if (options.visible)
            surface->window->open();
    }
    gui.surfaces.push_back(std::move(surface));
    const int result = protected_call(lua, [&](lua_State* lua) {
        *static_cast<Surface**>(lua_newuserdatauv(lua, sizeof(Surface*), 0)) = entry;
        luaL_setmetatable(lua, surface_metatable);
        lua_pushvalue(lua, -1);
        entry->handle = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }, 1);
    if (result < 0)
        close_surface(*entry, false);
    return result;
}

int surface_close(lua_State* lua) {
    close_surface(surface_handle(lua, true), true);
    return 0;
}

int surface_status(lua_State* lua) {
    auto& surface = surface_handle(lua, true);
    const auto size = surface.size;
    const bool closed = surface.closed, visible = surface.visible;
    const bool focus_grab = surface.window && surface.window->hasFocusGrab();
    const float scale = surface.window ? surface.window->scale() : 1.F;
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 5);
        lua_pushboolean(lua, closed);
        lua_setfield(lua, -2, "closed");
        lua_pushboolean(lua, visible);
        lua_setfield(lua, -2, "visible");
        lua_pushnumber(lua, size.x);
        lua_setfield(lua, -2, "width");
        lua_pushnumber(lua, size.y);
        lua_setfield(lua, -2, "height");
        lua_pushnumber(lua, scale);
        lua_setfield(lua, -2, "scale");
        lua_pushboolean(lua, focus_grab); lua_setfield(lua, -2, "focus_grab");
        return 1;
    }, 1);
}

int set_visible(lua_State* lua) {
    auto& surface = surface_handle(lua);
    luaL_checktype(lua, 2, LUA_TBOOLEAN);
    const bool visible = lua_toboolean(lua, 2);
    if (surface.visible == visible)
        return 0;
    if (surface.kind == ht::HT_WINDOW_POPUP) {
        if (visible)
            open_popup(surface);
        else
            hide_popup(surface);
        return 0;
    }
    surface.visible = visible;
    if (visible)
        surface.window->open();
    else {
        if (surface.popup)
            hide_popup(*surface.popup);
        surface.focus_pending = false;
        ++surface.focus_generation;
        surface.window->close();
    }
    return 0;
}

int grab_focus(lua_State* lua) {
    auto& surface = surface_handle(lua);
    surface.focus_pending = false;
    ++surface.focus_generation;
    if (!surface.visible || !surface.window || !surface.window->grabFocus()) {
        lua_pushnil(lua); lua_pushliteral(lua, "focus grabs require a visible native surface and Hyprland focus-grab support"); return 2;
    }
    lua_pushboolean(lua, true); return 1;
}

int release_focus(lua_State* lua) {
    auto& surface = surface_handle(lua);
    surface.focus_pending = false;
    ++surface.focus_generation;
    if (surface.window) surface.window->releaseFocus();
    return 0;
}

int set_input_region(lua_State* lua) {
    auto& surface = surface_handle(lua);
    std::optional<Hyprutils::Math::CRegion> region;
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        read_input_region(lua, 2, region);
        return 0;
    }, 0, 0, 1) < 0)
        return -1;
    if (surface.closed)
        throw std::runtime_error("surface closed while reading input region");
    if (surface.window && !surface.window->setInputRegion(region))
        throw std::runtime_error("toolkit surface input regions are unavailable");
    surface.input_region = std::move(region);
    return 0;
}

int set_size(lua_State* lua) {
    auto& surface = surface_handle(lua);
    const auto width = luaL_checkinteger(lua, 2), height = luaL_checkinteger(lua, 3);
    if ((surface.kind != ht::HT_WINDOW_TOPLEVEL && surface.kind != ht::HT_WINDOW_POPUP) || width < 1 || width > 16384 || height < 1 || height > 16384)
        return luaL_error(lua, "set_size requires a window or popup and dimensions in 1..16384");
    const Hyprutils::Math::Vector2D size{static_cast<double>(width), static_cast<double>(height)};
    if (surface.kind == ht::HT_WINDOW_POPUP) {
        if (surface.visible && !surface.window->repositionPopup(popup_position(surface, surface.popup_position), size))
            throw std::runtime_error("toolkit popup reposition failed");
        surface.popup_size = size;
    } else
        surface.window->setSize(size);
    return 0;
}

int reposition_popup(lua_State* lua) {
    auto& surface = surface_handle(lua);
    if (surface.kind != ht::HT_WINDOW_POPUP)
        return luaL_error(lua, "reposition requires a popup");
    const auto x = luaL_optinteger(lua, 2, static_cast<lua_Integer>(surface.popup_position.x));
    const auto y = luaL_optinteger(lua, 3, static_cast<lua_Integer>(surface.popup_position.y));
    if (x < -16384 || x > 16384 || y < -16384 || y > 16384)
        return luaL_error(lua, "popup offsets must be in -16384..16384");
    const Hyprutils::Math::Vector2D position{static_cast<double>(x), static_cast<double>(y)};
    if (surface.visible && !surface.window->repositionPopup(popup_position(surface, position), surface.popup_size))
        throw std::runtime_error("toolkit popup reposition failed");
    surface.popup_position = position;
    return 0;
}

}

void register_surfaces(lua_State* lua) {
    luaL_newmetatable(lua, surface_metatable);
    const luaL_Reg methods[] = {
        {"close", guarded<surface_close>}, {"status", guarded<surface_status>},
        {"set_visible", guarded<set_visible>}, {"set_size", guarded<set_size>},
        {"reposition", guarded<reposition_popup>}, {"set_input_region", guarded<set_input_region>},
        {"grab_focus", guarded<grab_focus>}, {"release_focus", guarded<release_focus>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, methods, 0);
    register_content(lua);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, guarded<surface_close>);
    lua_setfield(lua, -2, "__close");
    lua_pushliteral(lua, "hyprworks surface");
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
    const luaL_Reg functions[] = {
        {"window", guarded<create_surface<ht::HT_WINDOW_TOPLEVEL>>},
        {"layer", guarded<create_surface<ht::HT_WINDOW_LAYER>>},
        {"popup", guarded<create_surface<ht::HT_WINDOW_POPUP>>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
