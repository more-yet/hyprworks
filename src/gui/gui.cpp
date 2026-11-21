#include "../runtime/invocation.hpp"
#include "gui.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <unistd.h>

namespace hyprworks::lua {

namespace {

using Clock = std::chrono::steady_clock;

const xdg_wm_base_listener shell_listener{
    .ping = [](void*, xdg_wm_base* shell, std::uint32_t serial) { xdg_wm_base_pong(shell, serial); }
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
            if (bind(gui.shell, xdg_wm_base_interface, 6, 3))
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
            for (auto& surface : gui.surfaces) if (surface) {
                if (surface->layer && surface->output == name) request_surface_close(*surface);
                std::erase(surface->outputs, name);
            }
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
    if (!gui.compositor || !gui.shm || !gui.shell) throw std::runtime_error("compositor requires wl_compositor v4, wl_shm and xdg-shell v3");
    gui.images.start(state.wake_fd);
}

void close_gui(Gui& gui) noexcept {
    if (gui.stopping) return;
    gui.stopping = true;
    auto& state = gui.owner;
    if (gui.read_prepared) { wl_display_cancel_read(gui.display); gui.read_prepared = false; }
    gui.images.stop();
    if (gui.windows) {
        close_windows(*gui.windows);
        unref(state.lua, gui.windows->on_change); unref(state.lua, gui.windows->on_status);
    }
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
    if (gui.event_count || gui.outputs_pending || gui.focus_pending) deadline(Clock::time_point::min());
    for (const auto& seat : gui.seats) if (seat->repeat_at) deadline(*seat->repeat_at);
    for (const auto& surface : gui.surfaces) if (surface) {
        if (surface->closed || surface->close_pending || surface->configure_pending || surface->grab_pending ||
            (surface->mapped && surface->popup_reposition_pending) ||
            (surface->dirty && surface->configured && !surface->frame && !surface->waiting_buffer))
            deadline(Clock::time_point::min());
        for (const auto& element : surface->elements)
            if (element.image && !element.image_notified && element.image->ready.load(std::memory_order_acquire))
                deadline(Clock::time_point::min());
    }
    if (gui.windows) prepare_windows(*gui.windows, wait);
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
    if (gui.windows) read_windows(*gui.windows, wait);
}

int dispatch_gui(InvocationState& state) {
    if (!state.gui) return 0;
    auto& gui = *state.gui;
    check_display(gui);
    for (auto& surface : gui.surfaces)
        if (surface && surface->close_pending && dispatch_close(*surface) < 0) return -1;
    for (auto& surface : gui.surfaces)
        if (surface && !surface->closed && std::exchange(surface->grab_pending, false) &&
            notify(gui, surface->id, -1, surface->on_grab_cleared) < 0) return -1;
    if (gui.windows && dispatch_windows(*gui.windows) < 0) return -1;
    if (std::exchange(gui.outputs_pending, false) && notify(gui, 0, -1, gui.on_outputs) < 0) return -1;
    if (dispatch_input(gui) < 0) return -1;
    for (auto& surface : gui.surfaces) {
        if (!surface || surface->closed) continue;
        const auto id = surface->id;
        if (std::exchange(surface->configure_pending, false) && notify(gui, id, -1, surface->on_configure) < 0) return -1;
        if (surface->closed) continue;
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
    for (auto& surface : gui.surfaces)
        if (surface && !surface->closed && dispatch_scroll(*surface) < 0) return -1;
    gui.focus_pending = false;
    for (auto& surface : gui.surfaces) {
        if (!surface || surface->closed || surface->close_pending) continue;
        const auto current = surface_focus(*surface);
        if (surface->on_focus_change < 0) surface->reported_focus = current;
        else if (current != surface->reported_focus &&
            notify(gui, surface->id, -1, surface->on_focus_change) < 0) return -1;
    }
    for (auto& seat : gui.seats) update_ime(*seat);
    for (auto& surface : gui.surfaces) if (surface && surface->closed) destroy_surface(*surface);
    for (auto& surface : gui.surfaces) if (surface && surface->closed) surface.reset();
    std::erase_if(gui.buffers, [&](const auto& buffer) { return !buffer->busy && !find_surface(gui, buffer->surface); });
    for (auto& surface : gui.surfaces)
        if (surface && !state.quit_requested.load(std::memory_order_relaxed)) render(*surface);
    for (auto& surface : gui.surfaces)
        if (surface && !state.quit_requested.load(std::memory_order_relaxed)) reposition_popup(*surface);
    return 0;
}

void open_gui(lua_State* lua) {
    register_surfaces(lua);
    register_input(lua);
    const luaL_Reg functions[] = {
        {"outputs", operation_call<outputs>}, {"on_outputs", operation_call<on_outputs>}, {"capabilities", operation_call<capabilities>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, functions, 0);
}

}
