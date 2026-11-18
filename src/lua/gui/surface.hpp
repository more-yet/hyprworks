#pragma once

#include "../invocation.hpp"
#include <hyprtoolkit/window/Window.hpp>
#include <optional>

namespace hyprworks::lua {

namespace ht = Hyprtoolkit;
using Hyprutils::Memory::CSharedPointer;
enum class ElementKind { row, column, text, button, rectangle, checkbox, slider, progress, textbox, scroll, image, spinbox, combobox, radio, line, spacer };

struct Element {
    ElementKind kind = ElementKind::row;
    std::uint64_t generation = 0;
    std::size_t index = 0;
    unsigned depth = 1;
    std::string id;
    std::string group;
    CSharedPointer<ht::IElement> native;
    int callback = LUA_NOREF;
    std::size_t text_bytes = 0;
    std::size_t source_bytes = 0;
    std::size_t image_bytes = 0;
    std::size_t fixed_bytes = 0;
    std::uint64_t source = 1;
    float minimum = 0, maximum = 1, value = 0;
    bool enabled = true, focusable = false;
    bool programmatic = false;
};

struct Content {
    lua_State* lua;
    std::uint64_t generation;
    std::vector<Element> elements;
    std::size_t string_bytes = 0;
    std::size_t image_bytes = 0;
    ~Content();
};

struct Surface {
    InvocationState& owner;
    CSharedPointer<ht::IWindow> window;
    std::unique_ptr<Content> content;
    Hyprutils::Signal::CHyprSignalListener close_listener, role_listener, resize_listener, scale_listener, output_listener, focus_listener;
    int handle = LUA_NOREF;
    int on_close = LUA_NOREF;
    int on_resize = LUA_NOREF;
    int on_scale = LUA_NOREF;
    bool scale_pending = false;
    int on_focus_cleared = LUA_NOREF;
    bool focus_pending = false;
    std::uint64_t focus_generation = 0;
    ht::eWindowType kind = ht::HT_WINDOW_TOPLEVEL;
    Surface* parent = nullptr;
    Surface* popup = nullptr;
    Hyprutils::Math::Vector2D popup_position, popup_size;
    std::string anchor;
    std::optional<Hyprutils::Math::CRegion> input_region;
    struct Shortcut { std::uint32_t key = 0, modifiers = 0; int callback = LUA_NOREF; };
    std::vector<Shortcut> shortcuts;
    std::uint64_t epoch = 0;
    bool visible = false;
    bool closed = false;
    bool close_pending = false;
    bool notify_close = false;
    bool resize_pending = false;
    Hyprutils::Math::Vector2D size;
    std::uint64_t last_generation = 0;
    explicit Surface(InvocationState& state) : owner(state) {}
    ~Surface();
};

InvocationState& gui_state(lua_State* lua);
Surface& surface_handle(lua_State* lua, bool allow_closed = false);
void close_surface(Surface& surface, bool notify);
void hide_popup(Surface& surface);
void register_surfaces(lua_State* lua);
void register_content(lua_State* lua);
bool keyboard_input(Surface& surface, const ht::Input::SKeyboardKeyEvent& event) noexcept;
Element* event_element(Surface& surface, std::uint64_t generation, std::size_t index);
std::optional<ht::CHyprColor> color_field(lua_State* lua, int table, const char* name);

}
