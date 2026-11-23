#include "../src/runtime/invocation.hpp"
#include "../src/gui/gui.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <png.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace hyprworks::lua;
using Clock = std::chrono::steady_clock;

int main(int argc, char**) {
    if (argc > 1) {
        const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        assert(wake >= 0);
        ImageWork work;
        std::vector<std::shared_ptr<Image>> pending;
        for (int i = 0; i < 33; ++i) {
            auto job = std::make_shared<Image>(work);
            job->set_data({"\xff\0\0\x80", 4});
            job->width = job->height = 1; job->channels = 4;
            assert(work.submit(job) == (i < 32));
            pending.push_back(std::move(job));
        }
        assert(work.bytes.load() == 33 * 4);
        auto image = pending.back();
        pending.clear();
        assert(work.submit(image));
        assert(work.bytes.load() == 4);
        work.start(wake);
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!image->ready.load(std::memory_order_acquire)) {
            assert(Clock::now() < deadline);
            pollfd fd{wake, POLLIN, 0};
            assert(poll(&fd, 1, 100) >= 0);
            std::uint64_t value;
            if (read(wake, &value, sizeof(value)) < 0) assert(errno == EAGAIN || errno == EINTR);
        }
        assert(image->error.empty() && image->encoded.empty());
        cairo_surface_flush(image->surface);
        assert(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(image->surface)) == 0x80800000);
        image.reset();
        for (const int channels : {3, 4, 0}) {
            const int stride = channels == 3 ? 3 : 4;
            const std::array<unsigned, 5> alphas{0, 1, 127, 128, 255};
            std::string source(alphas.size() * static_cast<std::size_t>(stride), '\0');
            for (std::size_t i = 0; i < alphas.size(); ++i) {
                const unsigned a = alphas[i], c = 173;
                const auto offset = i * static_cast<unsigned>(stride);
                source[offset] = static_cast<char>(c);
                source[offset + 1] = static_cast<char>((c + a) & 255);
                source[offset + 2] = static_cast<char>(a);
                if (stride == 4) source[offset + 3] = static_cast<char>(a);
            }
            auto decoded = std::make_shared<Image>(work);
            decoded->width = 5; decoded->height = 1; decoded->channels = channels;
            if (channels) decoded->set_data(source);
            else {
                png_image png{};
                png.version = PNG_IMAGE_VERSION; png.width = 5; png.height = 1; png.format = PNG_FORMAT_RGBA;
                png_alloc_size_t size = 0;
                assert(png_image_write_to_memory(&png, nullptr, &size, 0, source.data(), 0, nullptr));
                std::string encoded(size, '\0');
                assert(png_image_write_to_memory(&png, encoded.data(), &size, 0, source.data(), 0, nullptr));
                decoded->set_data(std::string_view(encoded).substr(0, size));
                png_image_free(&png);
            }
            assert(work.submit(decoded));
            while (!decoded->ready.load(std::memory_order_acquire)) {
                pollfd fd{wake, POLLIN, 0};
                assert(poll(&fd, 1, 3000) > 0);
                std::uint64_t value;
                assert(read(wake, &value, sizeof(value)) == sizeof(value));
            }
            assert(decoded->error.empty() && decoded->encoded.empty());
            const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(decoded->surface));
            for (std::size_t i = 0; i < alphas.size(); ++i) {
                const unsigned a = alphas[i], c = 173;
                const unsigned alpha = stride == 4 ? a : 255;
                const auto expected = alpha << 24 | ((c * alpha + 127) / 255) << 16 |
                    ((((c + a) & 255) * alpha + 127) / 255) << 8 | (a * alpha + 127) / 255;
                assert(pixels[i] == expected);
            }
        }
        for (const int pixels : {0, 12}) {
            auto svg = std::make_shared<Image>(work);
            svg->pixels = pixels;
            svg->set_data(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 4 2"><path fill="red" d="M0 0h4v2H0z"/></svg>)");
            assert(work.submit(svg));
            while (!svg->ready.load(std::memory_order_acquire)) {
                pollfd fd{wake, POLLIN, 0};
                assert(poll(&fd, 1, 3000) > 0);
                std::uint64_t value;
                assert(read(wake, &value, sizeof(value)) == sizeof(value));
            }
            assert(svg->encoded.empty());
            if (pixels) {
                assert(svg->error.empty() && svg->surface && svg->width == 12 && svg->height == 6);
            } else {
                assert(!svg->error.empty() && !svg->surface && svg->bytes == 0);
            }
        }
        for (int i = 0; i < 200; ++i) {
            auto job = std::make_shared<Image>(work);
            job->set_data({"\xff\0\0\xff", 4});
            job->width = job->height = 1; job->channels = 4;
            assert(work.submit(job));
        }
        work.stop();
        assert(work.bytes.load() == 0);
        std::uint64_t value;
        while (read(wake, &value, sizeof(value)) > 0) {}
        ImageWork canceled;
        auto abandoned = std::make_shared<Image>(canceled);
        abandoned->set_data({"\0\0\0\0", 4});
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
    lua_newtable(state.lua); open_gui(state.lua);
    register_windows(state.lua); lua_setglobal(state.lua, "hw");
    push_handle(state.lua, {1, 0, -1}, surface_type); lua_setglobal(state.lua, "surface");
    const auto script = [&](const char* source) {
        if (luaL_dostring(state.lua, source) != LUA_OK) {
            std::cerr << lua_tostring(state.lua, -1) << '\n' << source << '\n'; std::abort();
        }
    };

    script(R"(
        notifications = 0
        assert(surface:set_content {
            kind = "column", background = "#ff0000",
            children = {
                {kind = "input", id = "input", font = "Sans 12", text = "éz", on_change = function()
                    notifications = notifications + 1
                end},
                {kind = "button", id = "replace", font = "Sans 12", text = "replace", on_activate = function()
                    assert(surface:set_content {kind = "text", id = "new", font = "Sans 12", text = "new tree"})
                end},
            },
        })
        input = assert(surface:find("input"))
        assert(input:focus())
        assert(not pcall(function()
            surface:set_content {kind = "column", children = {{kind = "unknown"}}}
        end))
        assert(input:state().text == "éz")
    )");
    layout(surface);
    auto& input = surface.elements[1];
    xkb_compose_state_feed(keyboard->compose, XKB_KEY_Multi_key);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_COMPOSING);
    focus(surface, -1);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_NOTHING);
    Input reverse_tab{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_ISO_Left_Tab, .pressed = true};
    assert(process_input(surface, reverse_tab) == -1 && surface.focused == 2);
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
    assert(process_input(surface, backspace) == 1);
    assert(input.text == "z");
    assert(surface.content_bytes <= max_content);

    Input stale{.type = Input::Type::ime, .surface = 1, .revision = surface.revision - 1};
    stale.element = 1; stale.commit_set = true; stale.text = "obsolete";
    assert(process_input(surface, stale) == -1 && input.text == "z");

    Input ime{.type = Input::Type::ime, .surface = 1, .revision = surface.revision};
    ime.element = 1; ime.commit_set = true; ime.text = "日本";
    assert(process_input(surface, ime) == 1 && input.text == "日本z");
    assert(not replace_text(surface, input, 1, 2, ""));
    assert(!replace_text(surface, input, 0, 0, std::string(16385, 'x')));
    input.cursor = 3; input.anchor = 6;
    ime.before = 3; ime.after = 1; ime.commit_set = false;
    assert(process_input(surface, ime) == 1 && input.text == "本" && input.cursor == 0 && input.anchor == 3);
    script("assert(input:set {text = '日本z'})");
    input.cursor = 3; input.anchor = 6; ime.commit_set = true;
    ime.before = 3; ime.after = 1; ime.text = "A";
    assert(process_input(surface, ime) == 1 && input.text == "A");
    ime.before = 1; ime.after = 0; ime.text.assign(16385, 'x');
    assert(process_input(surface, ime) == -1 && input.text == "A");
    const auto editing = input.edit_revision;
    script("assert(input:set {color = '#00ff00'})");
    assert(surface.elements[1].edit_revision == editing);
    focus(surface, 2); focus(surface, 1);
    assert(surface.elements[1].edit_revision > editing);
    input.cursor = 0; input.anchor = 1;
    ime.before = 0; ime.after = 0; ime.commit_set = false;
    ime.preedit_set = true; ime.preedit = "日本";
    ime.preedit_cursor = ime.preedit_end = 1;
    assert(process_input(surface, ime) == -1 && input.text == "A");
    ime.preedit_cursor = ime.preedit_end = 6;
    assert(process_input(surface, ime) == 1 && input.text.empty());
    assert(input.cursor == 0 && input.anchor == 0 && input.preedit == "日本");
    assert(process_input(surface, ime) == -1 && input.text.empty());
    Input ime_leave{.type = Input::Type::ime_leave, .surface = 1, .revision = surface.revision, .seat = 12, .element = 1};
    assert(process_input(surface, ime_leave) == -1 && input.preedit == "日本");
    ime_leave.seat = ime.seat;
    assert(process_input(surface, ime_leave) == -1 && input.preedit.empty());
    ime.preedit.clear(); ime.preedit_cursor = ime.preedit_end = 0;
    assert(process_input(surface, ime) == -1 && input.text.empty() && input.preedit.empty());
    script("assert(input:set {text = 'narrow', width = 1, padding = 12})");
    layout(surface);
    const auto caret = input_cursor(input, true);
    assert(std::isfinite(input.scroll) && input.scroll >= 0 && input.scroll <= std::max(0.0, caret.x / double(PANGO_SCALE)));
    script("assert(input:set {text = 'é日本', password = true, mask = '•', width = 200})");
    layout(surface);
    assert(process_input(surface, backspace) == 1 && input.text == "é日");
    Input left{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_Left, .pressed = true};
    assert(process_input(surface, left) == -1 && input.cursor == 2);
    ime.commit_set = true; ime.text = "secret";
    assert(process_input(surface, ime) == -1 && input.text == "é日");
    script("assert(input:set {text = 'éz'})");
    input.cursor = input.anchor = 3;
    assert(process_input(surface, backspace) == 1 && input.text == "z");
    script("assert(input:set {password = false})");

    layout(surface);
    const auto button = surface.elements[2].box;
    Input press{.type = Input::Type::button, .surface = 1, .seat = 11, .serial = 42,
        .key = BTN_LEFT, .x = button.x + button.w / 2, .y = button.y + button.h / 2, .pressed = true};
    assert(process_input(surface, press) == -1 && surface.pressed == 2);
    Input release = press; release.pressed = false; release.seat = 12;
    assert(process_input(surface, release) == -1 && surface.pressed == 2);
    release.seat = 11; release.touch = true;
    assert(process_input(surface, release) == -1 && surface.pressed == 2);
    Input leave{.type = Input::Type::leave, .surface = 1, .seat = 12};
    assert(process_input(surface, leave) == -1 && surface.pressed == 2);
    release.touch = false;
    assert(process_input(surface, release) == 2 && surface.pressed == -1 && gui.event_serial == 42);

    const auto old_revision = surface.revision;
    focus(surface, 1);
    xkb_compose_state_feed(keyboard->compose, XKB_KEY_Multi_key);
    assert(notify(gui, 1, 2, surface.elements[2].callback, "activate") == 0);
    assert(surface.revision != old_revision && surface.elements.size() == 1);
    assert(xkb_compose_state_get_status(keyboard->compose) == XKB_COMPOSE_NOTHING);
    script("assert(not input:set {text = 'stale'}); assert(notifications == 0)");

    script("assert(surface:set_content {kind = 'slider', horizontal = true, jump = true, width = 200, height = 30})");
    press.x = 0; press.y = 15;
    assert(process_input(surface, press) == -1 && surface.pressed == 0);
    Input motion{.type = Input::Type::motion, .surface = 1, .seat = 12, .x = 196, .y = 15};
    motion.x = surface.elements[0].box.x + surface.elements[0].box.w;
    assert(process_input(surface, motion) == -1 && surface.elements[0].value == 0);
    motion.seat = 11; motion.touch = true;
    assert(process_input(surface, motion) == -1 && surface.elements[0].value == 0);
    motion.touch = false;
    assert(process_input(surface, motion) == 0 && surface.elements[0].value == 1);
    push_handle(state.lua, {1, surface.revision, 0}, element_type); lua_setglobal(state.lua, "slider");
    script("assert(slider:set {enabled = false})");
    assert(surface.pressed == -1 && surface.focused == -1);
    motion.x = 4;
    assert(process_input(surface, motion) == -1 && surface.elements[0].value == 1);

    script(R"(
        assert(surface:set_content {kind = "column", children = {
            {kind = "box", id = "branch", children = {{kind = "text", id = "removed", font = "Sans 12", text = "old"}}},
            {kind = "input", id = "retained", font = "Sans 12", text = "selection"},
        }})
        branch = assert(surface:find("branch"))
        retained = assert(surface:find("retained"))
        assert(retained:focus())
        assert(not branch:replace_children {{kind = "text", id = "retained"}})
        assert(surface:find("removed"))
        assert(retained:state().text == "selection")
        assert(branch:replace_children {
            {kind = "button", id = "new", children = {{kind = "text", font = "Sans 12", text = "new"}}},
            {kind = "text", id = "tail", font = "Sans 12", text = "tail"},
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
        assert(surface:find("branch"):replace_children {{kind = "text", id = "inserted", font = "Sans 12", text = "inserted"}})
    )");
    assert(surface.focused == 3 && surface.elements[0].end == 4 && surface.elements[1].end == 3);
    const auto index_of = [&](std::string_view id) {
        for (std::size_t i = 0; i < surface.elements.size(); ++i)
            if (surface.elements[i].id == id) return static_cast<int>(i);
        std::abort();
    };
    script(R"(
        function editor_tree(reordered)
            local editor = {kind = "input", id = "editor", font = "Sans 12", text = "aé日z"}
            local button = {kind = "button", id = "action", font = "Sans 12", text = "Action"}
            return {kind = "column", id = "root", children = reordered and {
                {kind = "text", font = "Sans 12", text = "inserted"}, button, editor,
            } or {editor, button}}
        end
        assert(surface:set_content(editor_tree(false)))
        assert(surface:find("editor"):focus())
        old_editor = surface:find("editor")
    )");
    layout(surface);
    auto& editor = surface.elements[static_cast<std::size_t>(index_of("editor"))];
    editor.anchor = 1; editor.cursor = 6;
    const auto* editor_layout = editor.layout;
    const auto editor_revision = editor.edit_revision;
    script(R"(
        local tree = editor_tree(true)
        tree.background = "#123456"
        assert(surface:set_content(tree, {preserve_state = true}))
        assert(not old_editor:state())
    )");
    layout(surface);
    auto& moved_editor = surface.elements[static_cast<std::size_t>(index_of("editor"))];
    assert(surface.focused == index_of("editor") && moved_editor.layout == editor_layout);
    assert(moved_editor.anchor == 1 && moved_editor.cursor == 6 && moved_editor.edit_revision == editor_revision);
    surface.width = 480; dirty(surface); layout(surface);
    Input typing{.type = Input::Type::key, .surface = 1, .key = XKB_KEY_x, .pressed = true, .text = "x"};
    assert(process_input(surface, typing) == index_of("editor") && moved_editor.text == "axz");
    script(R"(
        assert(surface:find("action"):focus())
        assert(surface:set_content(editor_tree(false), {preserve_state = true}))
    )");
    assert(surface.focused == index_of("action"));
    script(R"(
        assert(surface:find("editor"):focus())
        local tree = editor_tree(true)
        tree.children[3].text = nil
        assert(surface:find("root"):replace_children(tree.children, {preserve_state = true}))
        assert(surface:find("editor"):state().text == "aé日z")
    )");
    auto& retained_editor = surface.elements[static_cast<std::size_t>(index_of("editor"))];
    retained_editor.cursor = 3; retained_editor.anchor = 1;
    script(R"(
        local tree = editor_tree(false)
        assert(surface:find("root"):replace_children(tree.children, {preserve_state = true}))
    )");
    assert(surface.focused == index_of("editor"));
    assert(surface.elements[static_cast<std::size_t>(surface.focused)].cursor == 3);
    assert(surface.elements[static_cast<std::size_t>(surface.focused)].anchor == 1);
    const auto transaction_revision = surface.revision;
    script(R"(
        assert(not surface:set_content({kind = "column", children = {
            {kind = "input", id = "editor", font = "Sans 12", text = "changed"}, {kind = "button", id = "editor"},
        }}, {preserve_state = true}))
        assert(not pcall(function()
            surface:find("root"):replace_children({{kind = "input", text = "\255"}}, {preserve_state = true})
        end))
        assert(not pcall(function() surface:find("editor"):set {text = "\0"} end))
        assert(not pcall(function() surface:find("editor"):set {accessibility = {name = "\255"}} end))
    )");
    assert(surface.revision == transaction_revision && surface.focused == index_of("editor"));
    assert(surface.elements[static_cast<std::size_t>(surface.focused)].cursor == 3);
    assert(process_input(surface, typing) == index_of("editor"));
    assert(surface.elements[static_cast<std::size_t>(surface.focused)].text == "ax日z");
    script(R"(
        local tree = editor_tree(false)
        tree.children[1].enabled = false
        assert(surface:set_content(tree, {preserve_state = true}))
    )");
    assert(surface.focused == -1);
    script(R"(
        assert(surface:set_content(editor_tree(false), {preserve_state = true}))
        assert(surface:find("editor"):focus())
        assert(surface:set_content({kind = "button", id = "editor"}, {preserve_state = true}))
    )");
    assert(surface.focused == -1);
    script(R"(
        activations = 0
        assert(surface:set_content {kind = "button", id = "pressed",
            on_activate = function() activations = activations + 1 end})
    )");
    press.x = 20; press.y = 20;
    process_input(surface, press);
    assert(surface.pressed == 0);
    enqueue(*keyboard, press); enqueue(*keyboard, release);
    enqueue(*keyboard, {.type = Input::Type::key, .surface = 1, .key = XKB_KEY_Return, .pressed = true});
    script(R"(
        assert(surface:set_content({kind = "button", id = "replacement",
            on_activate = function() activations = activations + 1 end}, {preserve_state = true}))
    )");
    assert(surface.pressed == -1 && surface.focused == -1);
    assert(dispatch_input(gui) == 0 && gui.event_count == 0);
    assert(process_input(surface, release) == -1);
    script("assert(activations == 0)");

    script(R"(
        assert(surface:set_content {kind = "row", children = {
            {kind = "button", id = "key", font = "Sans 12", text = "Activate", on_activate = function() activations = activations + 1 end},
            {kind = "toggle", id = "toggle"},
        }})
        assert(surface:find("key"):focus())
    )");
    Input key_press{.type = Input::Type::key, .surface = 1, .seat = 7, .serial = 50,
        .key = XKB_KEY_Return, .code = KEY_ENTER, .pressed = true};
    Input key_repeat = key_press; key_repeat.serial = 0; key_repeat.repeated = true;
    Input key_release = key_press; key_release.pressed = false;
    assert(process_input(surface, key_press) == -1 && surface.key_pressed == index_of("key"));
    assert(process_input(surface, key_repeat) == -1);
    key_release.seat = 12;
    assert(process_input(surface, key_release) == -1 && surface.key_pressed == index_of("key"));
    key_release.seat = 7; key_release.code = KEY_SPACE;
    assert(process_input(surface, key_release) == -1 && surface.key_pressed == index_of("key"));
    key_release.code = KEY_ENTER; key_release.key = XKB_KEY_space;
    assert(process_input(surface, key_release) == index_of("key") && surface.key_pressed == -1);
    assert(process_input(surface, key_release) == -1);
    assert(process_input(surface, key_repeat) == -1 && surface.key_pressed == -1);
    process_input(surface, key_press);
    focus(surface, index_of("toggle")); focus(surface, index_of("key"));
    assert(process_input(surface, key_release) == -1);
    process_input(surface, key_press);
    script("assert(surface:find('key'):set {enabled = false}); assert(surface:find('key'):set {enabled = true}); assert(surface:find('key'):focus())");
    assert(process_input(surface, key_release) == -1);
    process_input(surface, key_press);
    Input key_blur{.type = Input::Type::blur, .surface = 1, .seat = 7};
    process_input(surface, key_blur);
    assert(process_input(surface, key_release) == -1);
    process_input(surface, key_press);
    script(R"(
        assert(surface:set_content({kind = "button", id = "key", font = "Sans 12",
            on_activate = function() activations = activations + 1 end}, {preserve_state = true}))
    )");
    assert(surface.focused == 0 && process_input(surface, key_release) == -1);
    script(R"(
        function consume_release(_, event) return event.kind == "key" and not event.pressed end
    )");
    lua_getglobal(state.lua, "consume_release"); surface.on_input = luaL_ref(state.lua, LUA_REGISTRYINDEX);
    enqueue(*keyboard, key_press); enqueue(*keyboard, key_repeat); enqueue(*keyboard, key_release);
    assert(dispatch_input(gui) == 0 && surface.key_pressed == -1);
    script("assert(activations == 0)");
    unref(state.lua, surface.on_input);
    enqueue(*keyboard, key_press); enqueue(*keyboard, key_repeat); enqueue(*keyboard, key_release);
    assert(dispatch_input(gui) == 0);
    script(R"(
        assert(activations == 1)
        assert(surface:set_content {kind = "row", children = {
            {kind = "button", id = "key", on_activate = function() activations = activations + 1 end},
            {kind = "button", id = "other"},
        }})
        assert(surface:find("key"):focus())
        function redirect_key(_, event)
            if event.pressed then
                assert(surface:find("other"):focus())
                assert(surface:find("key"):focus())
            end
        end
    )");
    key_press.key = key_repeat.key = key_release.key = XKB_KEY_space;
    key_press.code = key_repeat.code = key_release.code = KEY_SPACE;
    lua_getglobal(state.lua, "redirect_key"); surface.on_input = luaL_ref(state.lua, LUA_REGISTRYINDEX);
    enqueue(*keyboard, key_press); enqueue(*keyboard, key_release);
    assert(dispatch_input(gui) == 0);
    script("assert(activations == 1)");
    unref(state.lua, surface.on_input);
    key_press.serial = 0;
    enqueue(*keyboard, key_press); enqueue(*keyboard, key_repeat); enqueue(*keyboard, key_release);
    assert(dispatch_input(gui) == 0);
    script("assert(activations == 2)");

    script(R"(
        nested_events = 0
        local function changed() nested_events = nested_events + 1 end
        assert(surface:set_content {kind = "box", children = {
            {kind = "scroll", id = "outer", horizontal = false, width = 100, height = 100, on_change = changed, children = {
                {kind = "column", height = 300, children = {
                    {kind = "scroll", id = "inner", horizontal = false, width = 100, height = 80, on_change = changed,
                        children = {{kind = "box", height = 100}}},
                    {kind = "box", height = 220},
                }},
            }},
            {kind = "scroll", id = "sibling", horizontal = false, x = 150, width = 100, height = 100,
                children = {{kind = "box", height = 300}}},
        }})
    )");
    Input nested_wheel{.type = Input::Type::axis, .surface = 1, .seat = 7, .x = 10, .y = 10, .delta = 35};
    process_input(surface, nested_wheel);
    assert(surface.elements[static_cast<std::size_t>(index_of("inner"))].scroll == 20);
    assert(surface.elements[static_cast<std::size_t>(index_of("outer"))].scroll == 15);
    assert(surface.elements[static_cast<std::size_t>(index_of("sibling"))].scroll == 0);
    assert(dispatch_scroll(surface) == 0);
    script("assert(nested_events == 2)");

    script(R"(
        scroll_events = {}
        function scroller(horizontal)
            return {kind = "scroll", id = "scroll", width = 100, height = 100, horizontal = horizontal,
                on_change = function(element, event)
                    assert(event.kind == "change" and element:state().scroll == event.scroll)
                    scroll_events[#scroll_events + 1] = event.scroll
                end,
                children = {{kind = "box", width = 400, height = 400}}}
        end
        assert(surface:set_content {kind = "box", children = {scroller(false)}})
    )");
    Input wheel{.type = Input::Type::axis, .surface = 1, .seat = 7, .x = 10, .y = 10, .delta = 30};
    process_input(surface, wheel);
    wheel.delta = 999; process_input(surface, wheel);
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 1 and scroll_events[1] == 300)");
    process_input(surface, wheel);
    assert(dispatch_scroll(surface) == 0);
    script(R"(
        assert(#scroll_events == 1)
        assert(surface:find("scroll"):set {scroll = 10})
        assert(surface:set_content({kind = "box", children = {scroller(true)}}, {preserve_state = true}))
        assert(surface:find("scroll"):state().scroll == 10)
    )");
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 1)");
    wheel.delta = 20; wheel.horizontal = true;
    process_input(surface, wheel);
    wheel.delta = -20; process_input(surface, wheel);
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 1)");
    wheel.delta = 50; process_input(surface, wheel);
    script(R"(
        assert(surface:set_content({kind = "box", children = {scroller(true)}}, {preserve_state = true}))
    )");
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 2 and scroll_events[2] == 60)");
    process_input(surface, wheel);
    script("assert(surface:find('scroll'):set {scroll = 5})");
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 2)");
    process_input(surface, wheel);
    script(R"(
        local tree = scroller(true)
        tree.width = 390
        assert(surface:set_content({kind = "box", children = {tree}}, {preserve_state = true}))
    )");
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 3 and scroll_events[3] == 10)");
    process_input(surface, wheel);
    script("assert(surface:set_content {kind = 'box'})");
    assert(dispatch_scroll(surface) == 0);
    script(R"(
        assert(#scroll_events == 3)
        local first, second = scroller(false), scroller(false)
        first.id, second.id = "first", "second"
        first.on_change = function()
            assert(surface:set_content {kind = "box"})
        end
        assert(surface:set_content {kind = "row", children = {first, second}})
    )");
    wheel.horizontal = false; wheel.delta = 20;
    process_input(surface, wheel);
    wheel.x = 110; process_input(surface, wheel);
    assert(dispatch_scroll(surface) == 0);
    script("assert(#scroll_events == 3)");
    {
        script(R"(
            drag_events = 0
            function drag_tree(horizontal)
                return {kind = "box", children = {
                    {kind = "slider", id = "drag", scroll_target = "viewport", horizontal = horizontal,
                        x = 130, y = 20, width = horizontal and 100 or 20, height = horizontal and 20 or 100,
                        jump = true, children = {{kind = "box", id = "thumb", thumb = true}}},
                    {kind = "scroll", id = "viewport", x = 30, y = 20,
                        width = 100, height = 100, horizontal = horizontal,
                        children = {{kind = "box", width = 400, height = 400}},
                        on_change = function(element)
                            drag_events = drag_events + 1
                            assert(element:replace_children({{kind = "box", width = 400, height = 400}}))
                        end},
                }}
            end
            assert(surface:set_content {kind = "box", id = "root", children = {drag_tree(false)}})
        )");
        layout(surface);
        const auto thumb = surface.elements[static_cast<std::size_t>(index_of("thumb"))].box;
        Input hover{.type = Input::Type::motion, .surface = 1, .seat = 7,
            .x = thumb.x + 1, .y = thumb.y + thumb.h / 2};
        Input grab = hover;
        grab.type = Input::Type::button; grab.key = BTN_LEFT; grab.pressed = true;
        process_input(surface, grab);
        assert(surface.pressed == index_of("drag"));
        const auto drag_revision = surface.revision;
        script(R"(
            local tree = drag_tree(false)
            tree.children[1].scroll_target = "missing"
            assert(not surface:set_content(tree, {preserve_state = true}))
            assert(not surface:find("drag"):replace_children {
                {kind = "box", thumb = true}, {kind = "box", thumb = true},
            })
            assert(not surface:find("drag"):replace_children {
                {kind = "box", thumb = true, children = {
                    {kind = "scroll", horizontal = false},
                }},
            })
        )");
        assert(surface.revision == drag_revision && surface.pressed == index_of("drag"));
        auto drag = hover;
        drag.x += 100; drag.y += 35;
        drag.seat = 8;
        process_input(surface, drag);
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll == 0);
        drag.seat = 7; drag.touch = true;
        process_input(surface, drag);
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll == 0);
        drag.touch = false;
        process_input(surface, drag);
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll > 0);
        assert(dispatch_scroll(surface) == 0);
        assert(surface.pressed == index_of("drag"));
        script("assert(drag_events == 1)");
        drag.y += 500;
        process_input(surface, drag);
        assert(dispatch_scroll(surface) == 0);
        layout(surface);
        const auto& scrolled = surface.elements[static_cast<std::size_t>(index_of("viewport"))];
        assert(scrolled.scroll == scrolled.scroll_limit);
        Input drop = drag;
        drop.type = Input::Type::button; drop.key = BTN_LEFT; drop.pressed = false;
        enqueue(*keyboard, drop);
        script(R"(
            assert(surface:find("root"):replace_children({
                {kind = "box", width = 1, height = 1}, drag_tree(false),
            }, {preserve_state = true}))
        )");
        assert(surface.pressed == index_of("drag") && gui.events[0].surface == 1);
        assert(dispatch_input(gui) == 0 && surface.pressed == -1);
        const auto stopped = surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll;
        drag.y = 0;
        process_input(surface, drag);
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll == stopped);

        script("assert(surface:find('viewport'):set {scroll = 0})");
        grab.y = 119;
        process_input(surface, grab);
        assert(surface.pressed == index_of("drag"));
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll > 0);
        script("assert(surface:find('viewport'):set {enabled = false})");
        assert(surface.pressed == -1);
        process_input(surface, grab);
        assert(surface.pressed == -1);

        script("assert(surface:set_content {kind = 'box', children = {drag_tree(true)}})");
        layout(surface);
        const auto horizontal = surface.elements[static_cast<std::size_t>(index_of("thumb"))].box;
        grab.x = horizontal.x + horizontal.w / 2; grab.y = horizontal.y + 1;
        process_input(surface, grab);
        assert(surface.pressed == index_of("drag"));
        drag.x = 500; drag.y = grab.y;
        process_input(surface, drag);
        assert(dispatch_scroll(surface) == 0);
        const auto& horizontal_scroll = surface.elements[static_cast<std::size_t>(index_of("viewport"))];
        assert(horizontal_scroll.scroll == horizontal_scroll.scroll_limit);
        Input cancel{.type = Input::Type::blur, .surface = 1, .seat = 7};
        process_input(surface, cancel);
        assert(surface.pressed == -1);

        script(R"(
            assert(surface:set_content {kind = "box", children = {
                drag_tree(false),
                {kind = "button", id = "cover", x = 130, y = 20, width = 20, height = 100},
            }})
        )");
        grab.x = thumb.x + 1; grab.y = thumb.y + 1;
        process_input(surface, grab);
        assert(surface.pressed == index_of("cover"));
        script("assert(surface:set_content {kind = 'box', children = {{kind = 'box', width = 80, children = {drag_tree(false)}}}})");
        process_input(surface, grab);
        assert(surface.pressed == -1);
        script(R"(
            local tree = drag_tree(false)
            tree.children[2].children = {{kind = "box", height = 10}}
            assert(surface:set_content(tree))
        )");
        process_input(surface, grab);
        process_input(surface, drag);
        assert(surface.elements[static_cast<std::size_t>(index_of("viewport"))].scroll == 0);
    }
    {
        surface.width = 480; surface.height = 160; surface.scale = 1.25;
        script(R"(
            assert(surface:set_content {kind = "box", background = "#203040", children = {
                {kind = "column", width = 120, children = {
                    {kind = "text", id = "paint", font = "Sans 12", color = "#ffffff", text = "Paint", width = 100, wrap = true},
                    {kind = "button", id = "below", font = "Sans 12", color = "#ffffff", text = "Below"},
                }},
                {kind = "text", id = "stable", font = "Sans 12", color = "#ffffff", text = "Unchanged shaping", x = 200, width = 220},
                {kind = "box", x = 30, y = 10, width = 40, height = 40, background = "#ff000060"},
            }})
        )");
        const auto buffer = [&] {
            auto value = std::make_unique<Buffer>();
            value->gui = &gui; value->surface = surface.id;
            value->width = static_cast<int>(std::ceil(surface.width * surface.scale));
            value->height = static_cast<int>(std::ceil(surface.height * surface.scale));
            value->cairo = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, value->width, value->height);
            assert(cairo_surface_status(value->cairo) == CAIRO_STATUS_SUCCESS);
            value->damage = {0, 0, double(surface.width), double(surface.height)};
            return value;
        };
        gui.buffers.push_back(buffer()); gui.buffers.push_back(buffer());
        auto& first = *gui.buffers[0]; auto& delayed = *gui.buffers[1];
        auto reference = buffer();
        paint_buffer(surface, first); paint_buffer(surface, delayed);
        delayed.busy = true;
        const auto size = static_cast<std::size_t>(cairo_image_surface_get_stride(delayed.cairo) * delayed.height);
        const auto* pixels = cairo_image_surface_get_data(delayed.cairo);
        const std::vector<unsigned char> submitted(pixels, pixels + size);
        const auto compare = [&](Buffer& repaired) {
            reference->damage = {0, 0, double(surface.width), double(surface.height)};
            paint_buffer(surface, *reference);
            assert(std::memcmp(cairo_image_surface_get_data(repaired.cairo),
                cairo_image_surface_get_data(reference->cairo), size) == 0);
        };
        auto& stable = surface.elements[static_cast<std::size_t>(index_of("stable"))];
        const auto shaping = pango_layout_get_serial(stable.layout);
        script("assert(surface:find('paint'):set {color = '#00ff00'})");
        assert(!surface.layout_dirty);
        const auto repaired = paint_buffer(surface, first);
        assert(repaired.w * repaired.h < first.width * first.height / 2);
        compare(first);
        script(R"(
            assert(surface:find("paint"):set {text = "Text that wraps across several lines"})
            assert(surface:find("below"):set {background = "#0055ff80"})
        )");
        paint_buffer(surface, first); compare(first);
        assert(pango_layout_get_serial(stable.layout) == shaping);
        script("assert(surface:find('paint'):set {text = 'Small', x = 5, y = 3})");
        paint_buffer(surface, first); compare(first);
        assert(std::memcmp(cairo_image_surface_get_data(delayed.cairo), submitted.data(), size) == 0);
        delayed.busy = false;
        paint_buffer(surface, delayed); compare(delayed);
        std::vector<Box> boxes;
        for (const auto& element : surface.elements) boxes.push_back(element.box);
        dirty(surface); layout(surface);
        for (std::size_t i = 0; i < boxes.size(); ++i) assert(surface.elements[i].box == boxes[i]);
        paint_buffer(surface, first); compare(first);
        script(R"(
            assert(surface:set_content {kind = "scroll", id = "viewport", horizontal = false, background = "#203040", children = {
                {kind = "column", children = {
                    {kind = "text", id = "moving", font = "Sans 12", color = "#ffffff", text = "Scroll contents", height = 100},
                    {kind = "input", id = "edit", font = "Sans 12", color = "#ffffff", text = "Editor", height = 100},
                    {kind = "box", height = 100, background = "#ff000080"},
                }},
            }})
        )");
        paint_buffer(surface, first);
        const auto scroll_shaping = pango_layout_get_serial(surface.elements[static_cast<std::size_t>(index_of("moving"))].layout);
        script("assert(surface:find('viewport'):set {scroll = 55}); assert(surface:find('edit'):focus())");
        paint_buffer(surface, first); compare(first);
        assert(pango_layout_get_serial(surface.elements[static_cast<std::size_t>(index_of("moving"))].layout) == scroll_shaping);
        script("assert(surface:find('edit'):set {text = 'Changed editor', font = 'Sans 16'})");
        paint_buffer(surface, first); compare(first);
        for (const auto* kind : {"input", "text"}) for (const bool preserve : {false, true}) {
            lua_pushstring(state.lua, kind); lua_setglobal(state.lua, "cached_kind");
            script(R"(
                cached_tree = {kind = "column", padding = 5, children = {
                    {kind = cached_kind, id = "cached", font = "Sans 12", color = "#ffffff", text = "aé日", placeholder = "Placeholder"},
                    {kind = "text", id = "wrapped", font = "Sans 12", color = "#ffffff", text = "Words that wrap onto multiple lines",
                        width = 90, wrap = true},
                }}
                assert(surface:set_content(cached_tree))
            )");
            paint_buffer(surface, first);
            for (const auto* change : {
                "{text = 'A longer replacement'}", "{font = 'Serif 18'}",
                "{text = '', placeholder = 'Changed placeholder'}", "{text = 'é日', password = true, mask = '•'}",
                "{password = false, padding = 12, width = 80}", "{align = 'right', wrap = true}",
            }) {
                script((std::string("change = ") + change + R"(
                    for key, value in pairs(change) do cached_tree.children[1][key] = value end
                )").c_str());
                script(preserve ? "assert(surface:set_content(cached_tree, {preserve_state = true}))" :
                    "assert(surface:find('cached'):set(change))");
                paint_buffer(surface, first);
                boxes.clear();
                for (const auto& element : surface.elements) boxes.push_back(element.box);
                script("assert(surface:set_content(cached_tree))");
                compare(first);
                for (std::size_t i = 0; i < boxes.size(); ++i) assert(surface.elements[i].box == boxes[i]);
            }
            for (const int width : {300, 480}) {
                surface.width = width; dirty(surface);
                paint_buffer(surface, first);
                boxes.clear();
                for (const auto& element : surface.elements) boxes.push_back(element.box);
                script("assert(surface:set_content(cached_tree))");
                compare(first);
                for (std::size_t i = 0; i < boxes.size(); ++i) assert(surface.elements[i].box == boxes[i]);
            }
        }
        gui.buffers.clear();
        surface.scale = 1;
    }
    {
        WindowCatalog catalog;
        int wayland[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) == 0);
        gui.display = wl_display_connect_to_fd(wayland[0]);
        assert(gui.display);
        catalog.gui = &gui; catalog.subscribed = true;
        catalog.identity_prefix = "session:1:";
        catalog.invalidation = 1;
        const std::string reply = R"([{"address":"0x123","stableId":"abc","class":"app","title":"title","mapped":true,"size":[640,480]}])"
            "\n\n\n" R"({"address":"0x123"})";
        int sockets[2];
        const auto begin_query = [&] {
            assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) == 0);
            catalog.fd = sockets[0]; catalog.query = WindowCatalog::Query::reading;
            catalog.queried_invalidation = catalog.invalidation;
            return WaitSet{{{sockets[0], POLLIN, POLLIN}}, {}};
        };
        auto wait = begin_query();
        const auto finish_query = [&] {
            read_windows(catalog, wait);
            assert(catalog.query == WindowCatalog::Query::synchronizing && catalog.sync);
            wl_callback_destroy(std::exchange(catalog.sync, nullptr));
            catalog.query = WindowCatalog::Query::complete;
            read_windows(catalog, wait);
        };
        const auto split = reply.size() / 2;
        assert(write(sockets[1], reply.data(), split) == static_cast<ssize_t>(split));
        read_windows(catalog, wait);
        assert(!catalog.records);
        ++catalog.invalidation;
        assert(write(sockets[1], reply.data() + split, reply.size() - split) == static_cast<ssize_t>(reply.size() - split));
        close(sockets[1]);
        finish_query();
        assert(!catalog.records && catalog.refresh_at && catalog.fd < 0);
        wait = begin_query();
        assert(write(sockets[1], reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()));
        close(sockets[1]);
        finish_query();
        assert(catalog.records && catalog.revision == 1 && catalog.records->size() == 1);
        const auto published = catalog.records;
        assert(published->front().id == "session:1:abc" && published->front().focused && published->front().width == 640);
        catalog.change_pending = false;
        wait = begin_query();
        assert(write(sockets[1], reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()));
        close(sockets[1]);
        finish_query();
        assert(!catalog.change_pending && catalog.revision == 1 && catalog.records == published);
        auto window_capture = std::make_shared<Capture>();
        window_capture->gui = &gui; window_capture->catalog = &catalog;
        auto workspace_capture = std::make_shared<Capture>();
        workspace_capture->gui = &gui;
        workspace_capture->next_frame = Clock::now();
        workspace_capture->constraints = true;
        gui.captures[0] = window_capture; gui.captures[1] = workspace_capture;
        wait = begin_query();
        assert(write(sockets[1], "invalid", 7) == 7);
        close(sockets[1]);
        finish_query();
        assert(catalog.phase == WindowCatalog::Phase::failed && !catalog.records);
        assert(published->front().id == "session:1:abc" && published->front().width == 640);
        assert(catalog.change_pending && catalog.status_pending);
        assert(window_capture->pending && window_capture->unavailable && window_capture->catalog == nullptr);
        assert(!workspace_capture->closed && !workspace_capture->error && workspace_capture->next_frame);
        WaitSet capture_wait;
        prepare_captures(gui, capture_wait);
        assert(capture_wait.deadline == Clock::time_point::min());
        close_windows(catalog);
        assert(!workspace_capture->closed && window_capture->pending);
        close_captures(gui);
        assert(workspace_capture->closed && window_capture->closed && !gui.captures[0] && !gui.captures[1]);
        auto* registry = wl_display_get_registry(gui.display);
        bind_captures(gui, registry, 91, "ext_image_copy_capture_manager_v1");
        bind_captures(gui, registry, 92, "hyprland_workspace_image_capture_source_manager_v1");
        bind_captures(gui, registry, 93, "ext_workspace_manager_v1");
        const auto manager_id = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(gui.capture_workspaces));
        const std::uint32_t target_events[] = {
            manager_id, 12u << 16, 0xff000000u,
            manager_id, (12u << 16) | 1u, 0xff000001u,
        };
        assert(write(wayland[1], target_events, sizeof(target_events)) == sizeof(target_events));
        assert(wl_display_dispatch(gui.display) >= 0);
        assert(gui.capture_groups.size() == 1 && gui.capture_targets.size() == 1);
        auto* group = gui.capture_groups.front().get();
        auto* target = gui.capture_targets.front().get();
        const auto& group_events = *reinterpret_cast<const ext_workspace_group_handle_v1_listener*>(
            wl_proxy_get_listener(reinterpret_cast<wl_proxy*>(group->native)));
        const auto& workspace_events = *reinterpret_cast<const ext_workspace_handle_v1_listener*>(
            wl_proxy_get_listener(reinterpret_cast<wl_proxy*>(target->native)));
        const auto& manager_events = *reinterpret_cast<const ext_workspace_manager_v1_listener*>(
            wl_proxy_get_listener(reinterpret_cast<wl_proxy*>(gui.capture_workspaces)));
        auto& output = gui.outputs.emplace_back(std::make_unique<Output>());
        output->id = 94; output->state.name = "screen";
        output->native = reinterpret_cast<wl_output*>(wl_proxy_create(reinterpret_cast<wl_proxy*>(gui.display), &wl_output_interface));
        gui.shm = reinterpret_cast<wl_shm*>(wl_proxy_create(reinterpret_cast<wl_proxy*>(gui.display), &wl_shm_interface));
        assert(output->native && gui.shm);
        group_events.output_enter(group, group->native, output->native);
        group_events.workspace_enter(group, group->native, target->native);
        workspace_events.name(target, target->native, "test");
        script(R"(
            capture_options = {name = "test", output = "screen", mode = "windows_only", max_width = 8, max_height = 8, max_fps = 1}
            capture_frames, capture_ended = 0, 0
            function captured(handle, event)
                if event.kind == "frame" then
                    capture_frames = capture_frames + 1
                    retained_source = retained_source or event.source
                else
                    assert(event.kind == "unavailable")
                    capture_ended = capture_ended + 1
                    handle:close()
                end
            end
            assert(not hw.capture_workspace(capture_options, captured))
        )");
        manager_events.done(&gui, gui.capture_workspaces);
        script("capture = assert(hw.capture_workspace(capture_options, captured))");
        auto capture = gui.captures[0];
        const auto& session_events = *reinterpret_cast<const ext_image_copy_capture_session_v1_listener*>(
            wl_proxy_get_listener(reinterpret_cast<wl_proxy*>(capture->session)));
        session_events.shm_format(capture.get(), capture->session, WL_SHM_FORMAT_ARGB8888);
        session_events.buffer_size(capture.get(), capture->session, 8, 4);
        session_events.shm_format(capture.get(), capture->session, WL_SHM_FORMAT_XRGB8888);
        assert(!capture->constraints && capture->format == WL_SHM_FORMAT_ARGB8888);
        session_events.done(capture.get(), capture->session);
        assert(dispatch_captures(gui) == 0 && capture->frame && gui.capture_bytes == 128);
        const auto& frame_events = *reinterpret_cast<const ext_image_copy_capture_frame_v1_listener*>(
            wl_proxy_get_listener(reinterpret_cast<wl_proxy*>(capture->frame)));
        std::fill_n(static_cast<std::uint32_t*>(capture->pixels), 32, 0x80402010u);
        frame_events.ready(capture.get(), capture->frame);
        assert(dispatch_captures(gui) == 0 && capture->image && !capture->deadline);
        auto retained = capture->image;
        const auto* retained_pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(retained->surface));
        assert(retained_pixels[0] == 0x80402010u);
        capture->next_frame = Clock::now();
        assert(dispatch_captures(gui) == 0 && capture->frame);
        session_events.buffer_size(capture.get(), capture->session, 4, 8);
        assert(!capture->constraints && capture->format == UINT32_MAX);
        frame_events.ready(capture.get(), capture->frame);
        assert(dispatch_captures(gui) == 0 && !capture->frame && capture->image == retained);
        script("assert(capture_frames == 1)");
        session_events.shm_format(capture.get(), capture->session, WL_SHM_FORMAT_XRGB8888);
        session_events.done(capture.get(), capture->session);
        assert(dispatch_captures(gui) == 0 && capture->frame && capture->buffer_width == 4 && capture->buffer_height == 8);
        for (unsigned failure = 1; failure <= 3; ++failure) {
            frame_events.failed(capture.get(), capture->frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
            assert(!capture->error && capture->constraint_failures == failure && capture->next_frame);
            capture->next_frame = Clock::now();
            assert(dispatch_captures(gui) == 0 && capture->frame);
        }
        std::fill_n(static_cast<std::uint32_t*>(capture->pixels), 32, 0x00204060u);
        frame_events.ready(capture.get(), capture->frame);
        assert(dispatch_captures(gui) == 0 && capture->image != retained && capture->constraint_failures == 0);
        assert(*reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(capture->image->surface)) == 0xff204060u);
        assert(retained_pixels[0] == 0x80402010u);
        script("assert(capture_frames == 2)");
        script(R"(
            retry_capture = assert(hw.capture_workspace(capture_options, function(handle, event)
                assert(event.kind == "error")
                retry_failed = true
                handle:close()
            end))
        )");
        auto retry = gui.captures[1];
        session_events.buffer_size(retry.get(), retry->session, 8, 4);
        session_events.shm_format(retry.get(), retry->session, WL_SHM_FORMAT_ARGB8888);
        session_events.done(retry.get(), retry->session);
        for (unsigned failure = 1; failure <= 4; ++failure) {
            retry->next_frame = Clock::now();
            assert(dispatch_captures(gui) == 0 && retry->frame && gui.capture_bytes == 256);
            frame_events.failed(retry.get(), retry->frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        }
        assert(retry->pending && retry->error && !retry->session && gui.capture_bytes == 128);
        assert(dispatch_captures(gui) == 0 && retry->closed);
        script("assert(retry_failed); retry_capture = nil");
        group_events.workspace_leave(group, group->native, target->native);
        group_events.removed(group, group->native);
        assert(!target->group && gui.capture_groups.empty());
        workspace_events.removed(target, target->native);
        assert(gui.capture_targets.empty() && capture->pending && capture->unavailable && gui.capture_bytes == 0);
        assert(dispatch_captures(gui) == 0 && capture->closed);
        script("assert(capture_ended == 1); capture = nil; retained_source = nil; collectgarbage('collect')");
        assert(retained_pixels[0] == 0x80402010u);
        retained.reset();
        assert(gui.images.bytes.load() == 0);
        close_captures(gui);
        wl_shm_destroy(std::exchange(gui.shm, nullptr));
        wl_output_destroy(output->native); gui.outputs.clear();
        wl_registry_destroy(registry);
        surface.configured = false;
        script(R"(
            assert(surface:set_content {kind = "row", children = {
                {kind = "button", id = "first"}, {kind = "button", id = "second"},
            }})
            assert(surface:find("first"):focus())
            function consume_key(_, event)
                if event.kind == "key" then assert(surface:find("second"):focus()) end
                return true
            end
        )");
        lua_getglobal(state.lua, "consume_key");
        surface.on_input = luaL_ref(state.lua, LUA_REGISTRYINDEX);
        enqueue(*keyboard, {.type = Input::Type::key, .surface = 1, .key = XKB_KEY_Tab, .pressed = true});
        assert(dispatch_gui(gui) == 0);
        assert(surface.focus_visible && surface.focused == index_of("second"));
        enqueue(*keyboard, {.type = Input::Type::button, .surface = 1, .pressed = true});
        assert(dispatch_gui(gui) == 0);
        assert(!surface.focus_visible && surface.focused == index_of("second"));
        unref(state.lua, surface.on_input);
        surface.configured = true;
        wl_display_disconnect(std::exchange(gui.display, nullptr));
        close(wayland[1]);
    }
    script("assert(surface:set_content {kind = 'box'})");
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
            kind = "button", text = "close", font = "Sans 12",
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
    std::cout << "GUI core: finalizer-safe callbacks, input cancellation, tree transactions, snapshot and clipboard ownership passed\n";
}
