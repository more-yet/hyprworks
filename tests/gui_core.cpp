#include "../src/lua/gui/gui.cpp"
#include <cassert>
#include <iostream>
#include <sys/eventfd.h>
#include <sys/socket.h>

using namespace hyprworks::lua;

int main(int argc, char**) {
    if (argc > 1) {
        const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        assert(wake >= 0);
        ImageWork work;
        std::vector<std::shared_ptr<Image>> pending;
        for (int i = 0; i < 33; ++i) {
            auto job = std::make_shared<Image>(work);
            job->encoded.assign("\xff\0\0\x80", 4);
            job->width = job->height = 1; job->channels = 4;
            assert(work.submit(job) == (i < 32));
            pending.push_back(std::move(job));
        }
        auto image = pending.back();
        pending.clear();
        assert(work.submit(image));
        work.start(wake);
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!image->ready.load(std::memory_order_acquire)) {
            assert(Clock::now() < deadline);
            pollfd fd{wake, POLLIN, 0};
            assert(poll(&fd, 1, 100) >= 0);
            std::uint64_t value; (void)read(wake, &value, sizeof(value));
        }
        assert(image->error.empty() && image->encoded.empty());
        cairo_surface_flush(image->surface);
        assert(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(image->surface)) == 0x80800000);
        image.reset();
        for (int i = 0; i < 200; ++i) {
            auto job = std::make_shared<Image>(work);
            job->encoded.assign("\xff\0\0\xff", 4);
            job->width = job->height = 1; job->channels = 4;
            assert(work.submit(job));
        }
        work.stop();
        assert(work.bytes.load() == 0);
        std::uint64_t value;
        while (read(wake, &value, sizeof(value)) > 0) {}
        ImageWork canceled;
        auto abandoned = std::make_shared<Image>(canceled);
        abandoned->encoded.assign(4, '\0');
        assert(canceled.submit(abandoned));
        abandoned.reset();
        canceled.start(wake);
        pollfd fd{wake, POLLIN, 0};
        assert(poll(&fd, 1, 3000) == 1);
        assert(canceled.bytes.load() == 0);
        canceled.stop();
        close(wake);
        std::cout << "Image worker: queue capacity, cancellation, publication and shutdown passed\n";
        return 0;
    }
    InvocationState state;
    state.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(state.wake_fd >= 0);
    state.lua = luaL_newstate();
    assert(state.lua);
    *static_cast<InvocationState**>(lua_getextraspace(state.lua)) = &state;
    luaL_openlibs(state.lua);
    state.gui = std::make_unique<Gui>(state);
    auto& gui = *state.gui;
    gui.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(gui.xkb);
    constexpr char compose[] = "<Multi_key> <a> <e> : \"æ\" ae\n";
    gui.compose = xkb_compose_table_new_from_buffer(gui.xkb, compose, sizeof(compose) - 1, "C.UTF-8",
        XKB_COMPOSE_FORMAT_TEXT_V1, XKB_COMPOSE_COMPILE_NO_FLAGS);
    assert(gui.compose);
    auto& keyboard = gui.seats.emplace_back(std::make_unique<Seat>());
    keyboard->gui = &gui; keyboard->id = 7; keyboard->keyboard_surface = 1;
    keyboard->compose = xkb_compose_state_new(gui.compose, XKB_COMPOSE_STATE_NO_FLAGS);
    assert(keyboard->compose);
    gui.fonts = pango_cairo_font_map_new();
    gui.text_context = pango_font_map_create_context(gui.fonts);
    gui.images.start(state.wake_fd);
    gui.surfaces[0] = std::make_unique<Surface>();
    auto& surface = *gui.surfaces[0];
    surface.gui = &gui; surface.id = ++gui.next_surface;
    surface.width = 320; surface.height = 160; surface.configured = true;
    lua_newtable(state.lua); open_gui(state.lua); lua_setglobal(state.lua, "hw");
    push_handle(state.lua, {1, 0, -1}, surface_type); lua_setglobal(state.lua, "surface");
    const auto script = [&](const char* source) {
        if (luaL_dostring(state.lua, source) != LUA_OK) {
            std::cerr << lua_tostring(state.lua, -1) << '\n'; std::abort();
        }
    };

    script(R"(
        notifications = 0
        assert(surface:set_content {
            kind = "column", background = "#ff0000",
            children = {
                {kind = "input", id = "input", text = "éz", on_change = function()
                    notifications = notifications + 1
                end},
                {kind = "button", id = "replace", text = "replace", on_activate = function()
                    assert(surface:set_content {kind = "text", id = "new", text = "new tree"})
                end},
            },
        })
        input = assert(surface:find("input"))
        assert(not pcall(function() hw.window {width = 0} end))
        assert(not surface:state().closed)
        assert(input:focus())
        assert(input:set {color = "#ffffffff"})
        assert(not pcall(function()
            surface:set_content {kind = "column", children = {{kind = "unknown"}}}
        end))
        assert(input:state().text == "éz")
    )");
    layout(surface);
    auto& input = surface.elements[1];
    const auto* input_layout = input.layout;
    script("assert(input:set {color = '#00ff00'})");
    layout(surface);
    assert(input.layout == input_layout);
    xkb_compose_state_feed(keyboard->compose, XKB_KEY_Multi_key);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_COMPOSING);
    focus(surface, -1);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_NOTHING);
    Input reverse_tab{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_ISO_Left_Tab, .pressed = true};
    assert(process_input(gui, reverse_tab) == -1 && surface.focused == 2);
    focus(surface, 1);
    keyboard->keymap = xkb_keymap_new_from_names(gui.xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(keyboard->keymap);
    keyboard->keys = xkb_state_new(keyboard->keymap);
    assert(keyboard->keys);
    keyboard->repeat_at = Clock::now();
    const int keymap_fd = eventfd(0, EFD_CLOEXEC);
    assert(keymap_fd >= 0);
    keyboard_listener.keymap(keyboard.get(), nullptr, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, keymap_fd, 0);
    assert(!keyboard->keymap && !keyboard->keys && !keyboard->repeat_at);
    assert(fcntl(keymap_fd, F_GETFD) == -1 && errno == EBADF);
    input.cursor = input.anchor = 3;
    Input backspace{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_BackSpace, .pressed = true};
    assert(process_input(gui, backspace) == 1);
    assert(input.text == "z");
    assert(surface.content_bytes <= max_content);

    Input stale{.type = Input::Type::ime, .surface = 1, .revision = surface.revision - 1};
    stale.element = 1; stale.commit_set = true; stale.text = "obsolete";
    assert(process_input(gui, stale) == -1 && input.text == "z");

    Input ime{.type = Input::Type::ime, .surface = 1, .revision = surface.revision};
    ime.element = 1; ime.commit_set = true; ime.text = "日本";
    assert(process_input(gui, ime) == 1 && input.text == "日本z");
    assert(not replace_text(surface, input, 1, 2, ""));
    assert(!replace_text(surface, input, 0, 0, std::string(16385, 'x')));
    input.cursor = 3; input.anchor = 6;
    ime.before = 3; ime.after = 1; ime.text = "A";
    assert(process_input(gui, ime) == 1 && input.text == "A");
    ime.before = 1; ime.after = 0; ime.text.assign(16385, 'x');
    assert(process_input(gui, ime) == -1 && input.text == "A");
    const auto editing = input.edit_revision;
    script("assert(input:set {color = '#00ff00'})");
    assert(surface.elements[1].edit_revision == editing);
    focus(surface, 2); focus(surface, 1);
    assert(surface.elements[1].edit_revision > editing);
    input.cursor = 0; input.anchor = 1;
    ime.before = 0; ime.after = 0; ime.commit_set = false;
    ime.preedit_set = true; ime.preedit = "日本";
    ime.preedit_cursor = ime.preedit_end = 1;
    assert(process_input(gui, ime) == -1 && input.text == "A");
    ime.preedit_cursor = ime.preedit_end = 6;
    assert(process_input(gui, ime) == 1 && input.text.empty());
    assert(input.cursor == 0 && input.anchor == 0 && input.preedit == "日本");
    assert(process_input(gui, ime) == -1 && input.text.empty());
    Input ime_leave{.type = Input::Type::ime_leave, .surface = 1, .revision = surface.revision, .seat = 12, .element = 1};
    assert(process_input(gui, ime_leave) == -1 && input.preedit == "日本");
    ime_leave.seat = ime.seat;
    assert(process_input(gui, ime_leave) == -1 && input.preedit.empty());
    ime.preedit.clear(); ime.preedit_cursor = ime.preedit_end = 0;
    assert(process_input(gui, ime) == -1 && input.text.empty() && input.preedit.empty());
    script("assert(input:set {text = 'narrow', width = 1, padding = 12})");
    layout(surface);
    const auto caret = input_cursor(input, true);
    assert(std::isfinite(input.scroll) && input.scroll >= 0 && input.scroll <= std::max(0.0, caret.x / double(PANGO_SCALE)));
    script("assert(input:set {text = 'é日本', password = true, width = 200})");
    layout(surface);
    assert(std::string(pango_layout_get_text(input.layout)) == "***");
    assert(layout_index(input, 5) == 2);
    assert(process_input(gui, backspace) == 1 && input.text == "é日");
    Input left{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_Left, .pressed = true};
    assert(process_input(gui, left) == -1 && input.cursor == 2);
    ime.commit_set = true; ime.text = "secret";
    assert(process_input(gui, ime) == -1 && input.text == "é日");
    script("assert(input:set {text = 'éz'})");
    input.cursor = input.anchor = 3;
    assert(process_input(gui, backspace) == 1 && input.text == "z");
    script("assert(input:set {password = false})");

    layout(surface);
    const auto button = surface.elements[2].box;
    Input press{.type = Input::Type::button, .surface = 1, .seat = 11, .serial = 42,
        .key = BTN_LEFT, .x = button.x + button.w / 2, .y = button.y + button.h / 2, .pressed = true};
    assert(process_input(gui, press) == -1 && surface.pressed == 2);
    Input release = press; release.pressed = false; release.seat = 12;
    assert(process_input(gui, release) == -1 && surface.pressed == 2);
    release.seat = 11; release.touch = true;
    assert(process_input(gui, release) == -1 && surface.pressed == 2);
    Input leave{.type = Input::Type::leave, .surface = 1, .seat = 12};
    assert(process_input(gui, leave) == -1 && surface.pressed == 2);
    release.touch = false;
    assert(process_input(gui, release) == 2 && surface.pressed == -1 && gui.event_serial == 42);

    const auto old_revision = surface.revision;
    focus(surface, 1);
    xkb_compose_state_feed(keyboard->compose, XKB_KEY_Multi_key);
    assert(notify(gui, 1, 2, surface.elements[2].callback, "activate") == 0);
    assert(surface.revision != old_revision && surface.elements.size() == 1);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_NOTHING);
    script("assert(not input:set {text = 'stale'}); assert(notifications == 0)");

    script("assert(surface:set_content {kind = 'slider', width = 200, height = 30})");
    press.x = 4; press.y = 15;
    assert(process_input(gui, press) == -1 && surface.pressed == 0);
    Input motion{.type = Input::Type::motion, .surface = 1, .seat = 12, .x = 196, .y = 15};
    motion.x = surface.elements[0].box.x + surface.elements[0].box.w - 4;
    assert(process_input(gui, motion) == -1 && surface.elements[0].value == 0);
    motion.seat = 11; motion.touch = true;
    assert(process_input(gui, motion) == -1 && surface.elements[0].value == 0);
    motion.touch = false;
    assert(process_input(gui, motion) == 0 && surface.elements[0].value == 1);
    push_handle(state.lua, {1, surface.revision, 0}, element_type); lua_setglobal(state.lua, "slider");
    script("assert(slider:set {enabled = false})");
    assert(surface.pressed == -1 && surface.focused == -1);
    motion.x = 4;
    assert(process_input(gui, motion) == -1 && surface.elements[0].value == 1);

    script(R"(
        assert(surface:set_content {
            kind = "image",
            source = {data = "\0\255\0\128", width = 1, height = 1, channels = 4},
        })
    )");
    auto image = surface.elements[0].image;
    assert(gui.images.submit(image));
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!image->ready.load(std::memory_order_acquire)) {
        assert(Clock::now() < deadline);
        pollfd fd{state.wake_fd, POLLIN, 0};
        assert(poll(&fd, 1, 100) >= 0);
        std::uint64_t value; (void)read(state.wake_fd, &value, sizeof(value));
    }
    assert(image->error.empty());
    cairo_surface_flush(image->surface);
    assert(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(image->surface)) == 0x80008000);
    assert(image->encoded.empty());

    script("assert(surface:set_content {kind = 'box', background = '#ff0000'})");
    image.reset();
    layout(surface);
    auto* pixels = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 160);
    auto* cr = cairo_create(pixels);
    paint(surface, cr); cairo_destroy(cr); cairo_surface_flush(pixels);
    assert(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(pixels)) == 0xffff0000);
    cairo_surface_destroy(pixels);

    script(R"(
        assert(surface:set_content {kind = "column", children = {
            {kind = "box", id = "branch", children = {{kind = "text", id = "removed", text = "old"}}},
            {kind = "input", id = "retained", text = "selection"},
        }})
        branch = assert(surface:find("branch"))
        retained = assert(surface:find("retained"))
        assert(retained:focus())
        assert(not branch:replace_children {{kind = "text", id = "retained"}})
        assert(surface:find("removed"))
        assert(retained:state().text == "selection")
        assert(branch:replace_children {
            {kind = "button", id = "new", children = {{kind = "text", text = "new"}}},
            {kind = "text", id = "tail", text = "tail"},
        })
        assert(not retained:set {text = "stale"})
        assert(not surface:find("removed"))
        assert(surface:find("retained"):state().text == "selection")
    )");
    assert(surface.focused == 5 && surface.elements[5].parent == 0);
    assert(surface.elements[0].end == 6 && surface.elements[1].end == 5 && surface.elements[2].end == 4);
    script(R"(
        assert(surface:find("branch"):replace_children {})
        assert(surface:find("retained"):state().text == "selection")
        assert(surface:find("branch"):replace_children {{kind = "text", id = "inserted", text = "inserted"}})
    )");
    assert(surface.focused == 3 && surface.elements[0].end == 4 && surface.elements[1].end == 3);
    script(R"(
        assert(surface:set_content {kind = "box", children = {
            {kind = "scroll", id = "scroll", horizontal = true, width = 100, height = 40, scroll = 50,
                halign = "right", valign = "bottom", x = -10, y = -5,
                children = {{kind = "box", width = 200, height = 30}}},
        }})
        local scroll = assert(surface:find("scroll")):state()
        assert(scroll.x == 210 and scroll.y == 115 and scroll.scroll == 50)
    )");
    assert(surface.elements[2].box.x == 160 && surface.elements[2].clip.x == 210 && surface.elements[2].clip.w == 100);
    gui.images.stop();
    assert(gui.images.bytes.load() == 0);

    int descriptors[2];
    assert(pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0);
    close(descriptors[0]);
    assert(write_pipe(descriptors[1], "x", 1) == -1 && errno == EPIPE);
    close(descriptors[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) == 0);
    auto* display = wl_display_connect_to_fd(descriptors[0]);
    assert(display);
    auto* offer_proxy = wl_proxy_create(reinterpret_cast<wl_proxy*>(display), &wl_data_offer_interface);
    assert(offer_proxy);
    Offer offer{reinterpret_cast<wl_data_offer*>(offer_proxy), "text/plain;charset=utf-8"};
    Seat clipboard_seat;
    clipboard_seat.gui = &gui; clipboard_seat.selection = &offer;
    paste(clipboard_seat, nullptr);
    assert(wl_display_flush(display) >= 0);
    char request[256];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    iovec vector{request, sizeof(request)};
    msghdr message{};
    message.msg_iov = &vector; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    assert(recvmsg(descriptors[1], &message, MSG_CMSG_CLOEXEC) > 0);
    const auto* rights = CMSG_FIRSTHDR(&message);
    assert(rights && rights->cmsg_level == SOL_SOCKET && rights->cmsg_type == SCM_RIGHTS);
    int writer;
    std::memcpy(&writer, CMSG_DATA(rights), sizeof(writer));
    assert(!(fcntl(writer, F_GETFL) & O_NONBLOCK));
    auto& transfer = gui.transfers.back();
    assert(fcntl(transfer.fd, F_GETFL) & O_NONBLOCK);
    assert(fcntl(transfer.fd, F_SETPIPE_SZ, 4096) >= 0);
    const std::string clipboard(1024 * 1024, 'x');
    std::thread source([&] {
        assert(write_pipe(writer, clipboard.data(), clipboard.size()) == static_cast<ssize_t>(clipboard.size()));
        close(writer);
    });
    while (transfer.fd >= 0) {
        pollfd ready{transfer.fd, POLLIN, 0};
        assert(poll(&ready, 1, 100) >= 0);
        transfer_io(transfer, ready.revents);
    }
    source.join();
    assert(transfer.error.empty() && transfer.data == clipboard);
    gui.transfers.clear();
    wl_proxy_destroy(offer_proxy); wl_display_disconnect(display); close(descriptors[1]);

    script(R"(
        assert(surface:set_content {
            kind = "button", text = "close",
            on_activate = function() surface:close() end,
        })
    )");
    assert(notify(gui, 1, 0, surface.elements[0].callback, "activate") == 0);
    assert(surface.closed);
    script("assert(surface:close()); assert(not surface:set_content {kind = 'text'})");
    close_gui(gui);
    lua_close(state.lua); state.lua = nullptr;
    state.gui.reset();
    close(state.wake_fd);
    std::cout << "GUI core: Unicode editing, stale IME, callback replacement/close, image ownership and worker shutdown passed\n";
}
