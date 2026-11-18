#include "surface.hpp"

#include <algorithm>

namespace hyprworks::lua {

InvocationState& gui_state(lua_State* lua) {
    auto& gui = invocation(lua);
    if (gui.closing)
        luaL_error(lua, "invocation is shutting down");
    if (gui.loop != EventLoop::toolkit)
        luaL_error(lua, "GUI authoring is unavailable in a headless invocation");
    return gui;
}

namespace {

void subscribe_outputs(InvocationState& gui) {
    const auto outputs = gui.backend->getOutputs();
    if (outputs.size() > 128) throw std::runtime_error("GUI output limit of 128 exceeded");
    std::erase_if(gui.outputs, [&](const auto& subscription) { return std::ranges::find(outputs, subscription.output) == outputs.end(); });
    for (const auto& output : outputs) {
        if (std::ranges::any_of(gui.outputs, [&](const auto& subscription) { return subscription.output == output; })) continue;
        const auto changed = [&gui]() noexcept { gui.outputs_pending = true; wake(gui); };
        gui.outputs.push_back({output, output->m_events.changed.listen(changed), output->m_events.removed.listen(changed)});
    }
}

int on_outputs(lua_State* lua) {
    auto& gui = gui_state(lua);
    gui_backend(gui);
    gui.outputs_pending = replace_callback(lua, 1, gui.on_outputs, gui.closing);
    if (gui.outputs_pending) wake(gui);
    return 0;
}

int outputs(lua_State* lua) {
    auto& gui = gui_state(lua);
    std::vector<std::pair<std::string, std::string>> records;
    for (const auto& output : gui_backend(gui)->getOutputs())
        records.emplace_back(output->port(), output->desc());
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, static_cast<int>(records.size()), 0);
        lua_Integer index = 1;
        for (const auto& [name, description] : records) {
            lua_createtable(lua, 0, 2);
            lua_pushlstring(lua, name.data(), name.size());
            lua_setfield(lua, -2, "name");
            lua_pushlstring(lua, description.data(), description.size());
            lua_setfield(lua, -2, "description");
            lua_rawseti(lua, -2, index++);
        }
        return 1;
    }, 1);
}

int set_palette(lua_State* lua) {
    auto& gui = gui_state(lua);
    luaL_checktype(lua, 1, LUA_TTABLE);
    const auto palette = gui_backend(gui)->getPalette();
    // Shell palettes must survive toolkit configuration reloads.
    auto replacement = *ht::CPalette::emptyPalette();
    replacement.m_colors = palette->m_colors;
    replacement.m_vars = palette->m_vars;
    lua_pushvalue(lua, 1);
    if (protected_call(lua, [&](lua_State* lua) {
        for (const auto& [name, color] : {
            std::pair{"background", &replacement.m_colors.background}, {"text", &replacement.m_colors.text},
            {"base", &replacement.m_colors.base}, {"alternate_base", &replacement.m_colors.alternateBase},
            {"bright_text", &replacement.m_colors.brightText}, {"link_text", &replacement.m_colors.linkText},
            {"accent", &replacement.m_colors.accent}, {"accent_secondary", &replacement.m_colors.accentSecondary}}) {
            if (const auto value = color_field(lua, 2, name)) *color = *value;
        }
        for (const auto& [name, value] : {
            std::pair{"font_size", &replacement.m_vars.fontSize}, {"small_font_size", &replacement.m_vars.smallFontSize},
            {"h1_size", &replacement.m_vars.h1Size}, {"h2_size", &replacement.m_vars.h2Size}, {"h3_size", &replacement.m_vars.h3Size}})
            *value = static_cast<int>(integer_field(lua, 2, name, *value, 1, 256));
        replacement.m_vars.smallRounding = static_cast<int>(integer_field(lua, 2, "rounding_small", replacement.m_vars.smallRounding, 0, 256));
        replacement.m_vars.bigRounding = static_cast<int>(integer_field(lua, 2, "rounding_large", replacement.m_vars.bigRounding, 0, 256));
        text_field(lua, 2, "font_family", replacement.m_vars.fontFamily, 256);
        text_field(lua, 2, "font_family_monospace", replacement.m_vars.fontFamilyMonospace, 256);
        text_field(lua, 2, "icon_theme", replacement.m_vars.iconTheme, 256);
        return 0;
    }, 0, 0, 1) < 0) return -1;
    if (gui.closing) throw std::runtime_error("GUI closed while reading palette");
    *palette = std::move(replacement);
    return 0;
}

}

CSharedPointer<ht::IBackend> gui_backend(InvocationState& gui) {
    if (!gui.backend) {
        gui.backend = ht::IBackend::create();
        if (!gui.backend)
            throw std::runtime_error("cannot initialize hyprtoolkit backend");
        gui.backend_destroying = gui.backend->m_events.destroying.listen([&gui]() noexcept {
            gui.gui_error = "toolkit backend stopped while the invocation was active";
            close_gui(gui);
            wake(gui);
        });
        gui.output_added = gui.backend->m_events.outputAdded.listen([&gui](auto) noexcept { gui.outputs_pending = true; wake(gui); });
        subscribe_outputs(gui);
    }
    return gui.backend;
}

void open_gui(lua_State* lua) {
    lua_createtable(lua, 0, 6);
    register_surfaces(lua);
    const luaL_Reg functions[] = {
        {"outputs", guarded<outputs>}, {"on_outputs", guarded<on_outputs>},
        {"set_palette", guarded<set_palette>}, {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

int dispatch_gui(InvocationState& gui) {
    if (gui.gui_error)
        throw std::runtime_error(gui.gui_error);
    if (std::exchange(gui.outputs_pending, false)) {
        subscribe_outputs(gui);
        const int reference = gui.on_outputs;
        if (reference >= 0 && !gui.quit_requested.load(std::memory_order_relaxed)) {
            lua_pushcfunction(gui.lua, traceback);
            if (protected_call(gui.lua, [&](lua_State* lua) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
                if (!gui.closing && gui.on_outputs == reference) lua_call(lua, 0, 0);
                return 0;
            }, 0, 1) < 0) return -1;
            lua_settop(gui.lua, 0);
        }
    }
    std::array<InvocationState::ControlEvent, 256> events{};
    events.swap(gui.events);
    const auto count = std::exchange(gui.event_count, 0);
    struct Delivery { Surface* surface; bool close; std::uint64_t epoch; bool scale = false, focus = false; std::uint64_t focus_generation = 0; };
    std::vector<Delivery> deliveries;
    for (const auto& surface : gui.surfaces) {
        if (surface->close_pending) {
            if (surface->kind == ht::HT_WINDOW_POPUP)
                hide_popup(*surface);
            else
                close_surface(*surface, true);
        }
        if (std::exchange(surface->notify_close, false))
            deliveries.push_back({surface.get(), true, surface->epoch});
        else if (std::exchange(surface->resize_pending, false) && surface->on_resize != LUA_NOREF)
            deliveries.push_back({surface.get(), false, surface->epoch});
        if (!surface->closed && std::exchange(surface->scale_pending, false) && surface->on_scale >= 0)
            deliveries.push_back({surface.get(), false, surface->epoch, true});
        if (!surface->closed && std::exchange(surface->focus_pending, false) && surface->on_focus_cleared >= 0)
            deliveries.push_back({surface.get(), false, surface->epoch, false, true, surface->focus_generation});
    }
    for (const auto& delivery : deliveries) {
        if (gui.quit_requested.load(std::memory_order_relaxed))
            break;
        auto& surface = *delivery.surface;
        if (!delivery.close && (surface.closed || surface.epoch != delivery.epoch))
            continue;
        if (delivery.focus && delivery.focus_generation != surface.focus_generation)
            continue;
        lua_pushcfunction(gui.lua, traceback);
        const int result = protected_call(gui.lua, [&](lua_State* lua) {
            const int reference = delivery.close ? surface.on_close : delivery.scale ? surface.on_scale : delivery.focus ? surface.on_focus_cleared : surface.on_resize;
            if (reference == LUA_NOREF)
                return 0;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
            if (delivery.close && surface.closed)
                unref(lua, surface.on_close);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, surface.handle);
            if (delivery.focus && (surface.closed || delivery.focus_generation != surface.focus_generation)) return 0;
            lua_call(lua, 1, 0);
            return 0;
        }, 0, 1);
        if (result < 0)
            return -1;
        lua_settop(gui.lua, 0);
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (gui.quit_requested.load(std::memory_order_relaxed))
            break;
        const auto& event = events[index];
        lua_pushcfunction(gui.lua, traceback);
        const int result = protected_call(gui.lua, [&](lua_State* lua) {
            if (event.surface->closed || event.epoch != event.surface->epoch)
                return 0;
            if (event.generation == 0) {
                lua_rawgeti(lua, LUA_REGISTRYINDEX, event.surface->shortcuts[event.element].callback);
                lua_rawgeti(lua, LUA_REGISTRYINDEX, event.surface->handle);
                lua_call(lua, 1, 0);
                return 0;
            }
            const auto* found = event_element(*event.surface, event.generation, event.element);
            if (!found || (event.source && event.source != found->source))
                return 0;
            const auto& element = *found;
            const auto kind = element.kind;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, element.callback);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, event.surface->handle);
            if (kind == ElementKind::checkbox || kind == ElementKind::radio || kind == ElementKind::image)
                lua_pushboolean(lua, event.value != 0);
            else if (kind == ElementKind::slider)
                lua_pushnumber(lua, event.value);
            else if (kind == ElementKind::spinbox || kind == ElementKind::combobox)
                lua_pushinteger(lua, static_cast<lua_Integer>(event.value));
            else if (kind == ElementKind::textbox)
                lua_pushlstring(lua, event.text.data(), event.text.size());
            // Allocating a text argument can run finalizers that replace or close content.
            found = event_element(*event.surface, event.generation, event.element);
            if (!found || (event.source && event.source != found->source))
                return 0;
            lua_call(lua, kind == ElementKind::button ? 1 : 2, 0);
            return 0;
        }, 0, 1);
        if (result < 0)
            return -1;
        lua_settop(gui.lua, 0);
    }
    return 0;
}

void close_gui(InvocationState& gui) noexcept {
    gui.closing = true;
    gui.backend_destroying.reset();
    for (const auto& surface : gui.surfaces) {
        close_surface(*surface, false);
        unref(gui.lua, surface->handle);
    }
    gui.event_count = 0;
    gui.events = {};
    gui.output_added.reset(); gui.outputs.clear(); unref(gui.lua, gui.on_outputs); gui.outputs_pending = false;
    if (gui.backend) {
        gui.backend->destroy();
        gui.backend.reset();
    }
}

}
