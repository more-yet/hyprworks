#include "surface.hpp"

#include <hyprtoolkit/element/RowLayout.hpp>
#include <hyprtoolkit/element/ColumnLayout.hpp>
#include <hyprtoolkit/element/Text.hpp>
#include <hyprtoolkit/element/Button.hpp>
#include <hyprtoolkit/element/Checkbox.hpp>
#include <hyprtoolkit/element/Slider.hpp>
#include <hyprtoolkit/element/ProgressBar.hpp>
#include <hyprtoolkit/element/Rectangle.hpp>
#include <hyprtoolkit/element/Textbox.hpp>
#include <hyprtoolkit/element/ScrollArea.hpp>
#include <hyprtoolkit/element/Image.hpp>
#include <hyprtoolkit/element/Spinbox.hpp>
#include <hyprtoolkit/element/Combobox.hpp>
#include <hyprtoolkit/element/Line.hpp>
#include <hyprtoolkit/element/Null.hpp>
#include <hyprtoolkit/system/Icons.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace hyprworks::lua {

Content::~Content() {
    for (auto& element : elements) {
        element.native.reset();
        unref(lua, element.callback);
    }
}

Element* event_element(Surface& surface, std::uint64_t generation, std::size_t index) {
    if (surface.content) {
        for (auto& element : surface.content->elements) {
            if (element.generation == generation && element.index == index)
                return &element;
        }
    }
    return nullptr;
}

namespace {

constexpr std::size_t max_nodes = 2048;
constexpr std::size_t max_text_bytes = 16384;
constexpr std::size_t max_content_bytes = 1024 * 1024;

bool valid_input_text(std::string_view text) noexcept {
    if (text.size() > max_text_bytes)
        return false;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        if (lead == 0)
            return false;
        if (lead < 0x80)
            continue;
        unsigned remaining, minimum, value;
        if (lead >= 0xc2 && lead <= 0xdf) {
            remaining = 1; minimum = 0x80; value = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            remaining = 2; minimum = 0x800; value = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            remaining = 3; minimum = 0x10000; value = lead & 0x07;
        } else
            return false;
        if (remaining > text.size() - index)
            return false;
        for (unsigned part = 0; part < remaining; ++part) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

void queue_control(Surface& surface, std::uint64_t generation, std::size_t index,
                   float value = 0, std::string_view text = {}, std::uint64_t source = 0) noexcept {
    auto& gui = surface.owner;
    if (gui.closing || gui.gui_error || surface.closed)
        return;
    auto* found = event_element(surface, generation, index);
    if (!found || (source && found->source != source))
        return;
    auto& content = *surface.content;
    auto& element = *found;
    if (element.kind == ElementKind::slider) {
        element.value = value;
        if (element.programmatic)
            return;
    } else if (element.kind == ElementKind::textbox) {
        if (!valid_input_text(text)) {
            gui.gui_error = "textbox input exceeds 16 KiB or is not valid UTF-8 without NUL";
            wake(gui);
            return;
        }
        const auto bytes = content.string_bytes - element.text_bytes + text.size();
        if (bytes > max_content_bytes) {
            gui.gui_error = "content strings exceed 1 MiB";
            wake(gui);
            return;
        }
        content.string_bytes = bytes;
        element.text_bytes = text.size();
    }
    if (element.kind == ElementKind::radio && value != 0) {
        for (auto& other : content.elements) {
            if (&other != &element && other.kind == ElementKind::radio && other.group == element.group)
                static_cast<ht::CCheckboxElement*>(other.native.get())->setState(false);
        }
    }
    if (element.callback == LUA_NOREF)
        return;
    try {
        if ((element.kind == ElementKind::slider || element.kind == ElementKind::textbox) && gui.event_count > 0) {
            auto& previous = gui.events[gui.event_count - 1];
            if (previous.surface == &surface && previous.generation == generation && previous.element == index && previous.epoch == surface.epoch) {
                previous.text.assign(text);
                previous.value = value;
                return;
            }
        }
        if (gui.event_count == gui.events.size())
            gui.gui_error = "GUI control event queue limit reached";
        else {
            auto& event = gui.events[gui.event_count];
            event.text.assign(text);
            event.surface = &surface;
            event.generation = generation;
            event.element = index;
            event.source = source;
            event.epoch = surface.epoch;
            event.value = value;
            ++gui.event_count;
        }
    } catch (...) {
        gui.gui_error = "cannot retain GUI control event";
    }
    wake(gui);
}

float number_field(lua_State* lua, int table, const char* name, float fallback, float minimum, float maximum) {
    field(lua, table, name);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return fallback;
    }
    const auto value = luaL_checknumber(lua, -1);
    if (!std::isfinite(value) || value < minimum || value > maximum)
        luaL_error(lua, "%s is out of range", name);
    lua_pop(lua, 1);
    return static_cast<float>(value);
}

float control_value(lua_State* lua, lua_Number number, const Element& element) {
    if (!std::isfinite(number) || number < -1000000 || number > 1000000)
        luaL_error(lua, "value is outside the control range");
    const auto value = static_cast<float>(number);
    if (value < element.minimum || value > element.maximum)
        luaL_error(lua, "value is outside the control range");
    return value;
}

}

std::optional<ht::CHyprColor> color_field(lua_State* lua, int table, const char* name) {
    field(lua, table, name);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return std::nullopt;
    }
    luaL_checktype(lua, -1, LUA_TSTRING);
    std::size_t length;
    const char* text = lua_tolstring(lua, -1, &length);
    if ((length != 7 && length != 9) || text[0] != '#')
        luaL_error(lua, "%s must be #RRGGBB or #RRGGBBAA", name);
    std::uint32_t color = 0;
    const auto parsed = std::from_chars(text + 1, text + length, color, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text + length)
        luaL_error(lua, "%s must be #RRGGBB or #RRGGBBAA", name);
    if (length == 7)
        color = (color << 8) | 0xff;
    lua_pop(lua, 1);
    return ht::CHyprColor{static_cast<float>((color >> 24) & 0xff) / 255.F,
                         static_cast<float>((color >> 16) & 0xff) / 255.F,
                         static_cast<float>((color >> 8) & 0xff) / 255.F,
                         static_cast<float>(color & 0xff) / 255.F};
}

namespace {

struct ImageSource {
    std::string path, icon, fallback_icon;
    std::vector<std::uint8_t> data;
    int width = 0, height = 0;
};

void read_image_source(lua_State* lua, int table, ImageSource& source) {
    text_field(lua, table, "path", source.path);
    text_field(lua, table, "icon", source.icon);
    text_field(lua, table, "fallback_icon", source.fallback_icon);
    field(lua, table, "data");
    if (!lua_isnil(lua, -1)) {
        std::size_t size; const auto* data = reinterpret_cast<const std::uint8_t*>(luaL_checklstring(lua, -1, &size));
        if (!size || size > 8 * 1024 * 1024) luaL_error(lua, "encoded image must contain 1 byte..8 MiB");
        source.data.assign(data, data + size);
    }
    lua_pop(lua, 1);
    field(lua, table, "pixels");
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TTABLE);
        if (!source.data.empty()) luaL_error(lua, "image data and pixels are mutually exclusive");
        const int image = lua_absindex(lua, -1);
        source.width = static_cast<int>(integer_field(lua, image, "width", 0, 1, 4096));
        source.height = static_cast<int>(integer_field(lua, image, "height", 0, 1, 4096));
        const int channels = static_cast<int>(integer_field(lua, image, "channels", 4, 3, 4));
        const auto stride = static_cast<std::size_t>(integer_field(lua, image, "row_stride", source.width * channels, source.width * channels, 8 * 1024 * 1024));
        field(lua, image, "pixels");
        std::size_t size; const auto* data = reinterpret_cast<const std::uint8_t*>(luaL_checklstring(lua, -1, &size));
        const auto output_size = static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4;
        if (size > 8 * 1024 * 1024 || output_size > 8 * 1024 * 1024 || stride * static_cast<std::size_t>(source.height - 1) + static_cast<std::size_t>(source.width * channels) > size)
            luaL_error(lua, "invalid image pixels or 8 MiB budget exceeded");
        source.data.resize(output_size);
        for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
            const auto from = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x * channels);
            const auto to = (static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) + static_cast<std::size_t>(x)) * 4;
            std::copy_n(data + from, 3, source.data.data() + to);
            source.data[to + 3] = channels == 4 ? data[from + 3] : 255;
        }
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    if (static_cast<int>(!source.path.empty()) + static_cast<int>(!source.icon.empty()) + static_cast<int>(!source.data.empty()) != 1)
        luaL_error(lua, "image requires exactly one path, icon, data or pixels source");
    if (!source.fallback_icon.empty() && source.icon.empty())
        luaL_error(lua, "fallback_icon requires an icon source");
}

void apply_image_source(ht::CImageBuilder& builder, InvocationState& gui, ImageSource& source) {
    if (!source.data.empty()) {
        if (source.width) builder.pixels(std::move(source.data), source.width, source.height);
        else builder.data(std::move(source.data));
        return;
    }
    if (!source.path.empty()) {
        builder.path(std::move(source.path));
        return;
    }
    const auto factory = gui.backend->systemIcons();
    auto icon = factory->lookupIcon(source.icon);
    if ((!icon || !icon->exists()) && !source.fallback_icon.empty())
        icon = factory->lookupIcon(source.fallback_icon);
    if (!icon || !icon->exists())
        throw std::runtime_error("image icon not found: " + source.icon);
    builder.icon(icon);
}

struct NodeOptions {
    std::string text;
    std::string font_family;
    std::string placeholder;
    std::string tooltip;
    std::vector<std::string> items;
    std::vector<Hyprutils::Math::Vector2D> points;
    ImageSource image;
    int width = 0, height = 0;
    int gap = 0, margin = 0;
    int radius = 0, border_width = 0;
    float opacity = 1, font_size = 0;
    std::optional<ht::CHyprColor> color, border_color;
    ht::eFontAlignment align = ht::HT_FONT_ALIGN_LEFT;
    ht::eImageFitMode fit = ht::IMAGE_FIT_MODE_CONTAIN;
    bool enabled = true, accent = false;
    bool ellipsize = true, no_background = false, no_border = false;
    bool multiline = false, password = false, eye_icon = false;
    bool scroll_x = false, scroll_y = true, scrollbar = true;
    bool grow_x = false, grow_y = false, absolute = false;
    int horizontal = 0, vertical = 0, x = 0, y = 0;
    int thickness = 1;
};

int dimension(lua_State* lua, int table, const char* name, int fallback = 0) {
    field(lua, table, name);
    int value = fallback;
    if (lua_type(lua, -1) == LUA_TSTRING) {
        static const char* sizes[] = {"auto", "fill", nullptr};
        value = -luaL_checkoption(lua, -1, nullptr, sizes);
    } else if (!lua_isnil(lua, -1)) {
        const auto number = luaL_checkinteger(lua, -1);
        if (number < 1 || number > 16384)
            luaL_error(lua, "%s must be 1..16384, auto, or fill", name);
        value = static_cast<int>(number);
    }
    lua_pop(lua, 1);
    return value;
}

void build_node(lua_State* lua, int table, std::size_t parent, unsigned depth,
                Surface& surface, Content& content, NodeOptions& node) {
    if (depth > 32 || content.elements.size() == max_nodes)
        luaL_error(lua, "content exceeds 32 levels or 2048 elements");
    if (!lua_checkstack(lua, 8))
        luaL_error(lua, "cannot grow content parser stack");
    luaL_checktype(lua, table, LUA_TTABLE);
    table = lua_absindex(lua, table);
    const auto index = content.elements.size();
    content.elements.emplace_back();
    auto& entry = content.elements.back();
    entry.generation = content.generation;
    entry.index = index;
    entry.depth = depth;
    node = {};
    static const char* kinds[] = {"row", "column", "text", "button", "rectangle", "checkbox", "slider", "progress", "textbox", "scroll", "image", "spinbox", "combobox", "radio", "line", "spacer", nullptr};
    const auto kind = static_cast<ElementKind>(enum_field(lua, table, "kind", nullptr, kinds));
    entry.kind = kind;
    text_field(lua, table, "id", entry.id, 128);
    if (!entry.id.empty()) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (content.elements[previous].id == entry.id)
                luaL_error(lua, "duplicate element id");
        }
    }
    int width = 0, height = 0;
    if (kind == ElementKind::slider) { width = 200; height = 28; }
    else if (kind == ElementKind::progress) { width = 200; height = 8; }
    else if (kind == ElementKind::textbox) { width = 240; height = 32; }
    else if (kind == ElementKind::scroll) { width = -1; height = 200; }
    else if (kind == ElementKind::image) { width = 64; height = 64; }
    else if (kind == ElementKind::line) { width = 200; height = 1; }
    node.width = dimension(lua, table, "width", width);
    node.height = dimension(lua, table, "height", height);
    node.margin = static_cast<int>(integer_field(lua, table, "margin", 0));
    node.opacity = number_field(lua, table, "opacity", 1, 0, 1);
    text_field(lua, table, "tooltip", node.tooltip);
    node.grow_x = boolean_field(lua, table, "grow_x", kind == ElementKind::spacer);
    node.grow_y = boolean_field(lua, table, "grow_y", kind == ElementKind::spacer);
    static const char* horizontal[] = {"auto", "left", "center", "right", nullptr};
    static const char* vertical[] = {"auto", "top", "center", "bottom", nullptr};
    static const char* positions[] = {"auto", "absolute", nullptr};
    node.horizontal = enum_field(lua, table, "halign", "auto", horizontal);
    node.vertical = enum_field(lua, table, "valign", "auto", vertical);
    node.absolute = enum_field(lua, table, "position", "auto", positions) == 1;
    node.x = static_cast<int>(integer_field(lua, table, "x", 0, -16384));
    node.y = static_cast<int>(integer_field(lua, table, "y", 0, -16384));
    entry.focusable = boolean_field(lua, table, "focusable", kind == ElementKind::button || kind == ElementKind::checkbox ||
        kind == ElementKind::radio || kind == ElementKind::slider || kind == ElementKind::textbox || kind == ElementKind::spinbox || kind == ElementKind::combobox);
    if (kind == ElementKind::row || kind == ElementKind::column)
        node.gap = static_cast<int>(integer_field(lua, table, "gap", 0));
    if (kind == ElementKind::text || kind == ElementKind::button || kind == ElementKind::textbox || kind == ElementKind::spinbox)
        text_field(lua, table, "text", node.text);
    entry.text_bytes = node.text.size();
    if (kind == ElementKind::text || kind == ElementKind::button) {
        text_field(lua, table, "font_family", node.font_family);
        node.font_size = number_field(lua, table, "font_size", 0, 1, 256);
        static const char* alignments[] = {"left", "center", "right", nullptr};
        node.align = static_cast<ht::eFontAlignment>(enum_field(lua, table, "align", kind == ElementKind::button ? "center" : "left", alignments));
        node.ellipsize = boolean_field(lua, table, "ellipsize", kind == ElementKind::text);
    }
    if (kind == ElementKind::text || kind == ElementKind::rectangle || kind == ElementKind::line)
        node.color = color_field(lua, table, "color");
    if (kind == ElementKind::rectangle) {
        node.border_color = color_field(lua, table, "border_color");
        node.border_width = static_cast<int>(integer_field(lua, table, "border_width", 0));
        node.radius = static_cast<int>(integer_field(lua, table, "radius", 0));
    }
    if (kind == ElementKind::button) {
        node.enabled = boolean_field(lua, table, "enabled", true);
        entry.enabled = node.enabled;
        node.accent = boolean_field(lua, table, "accent", false);
        node.no_background = boolean_field(lua, table, "no_background", false);
        node.no_border = boolean_field(lua, table, "no_border", false);
        callback_field(lua, table, "on_click", entry.callback);
    } else if (kind == ElementKind::checkbox || kind == ElementKind::radio) {
        entry.value = boolean_field(lua, table, "value", false) ? 1.F : 0.F;
        if (kind == ElementKind::radio) {
            text_field(lua, table, "group", entry.group, 128);
            if (entry.group.empty())
                luaL_error(lua, "radio requires a nonempty group");
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto& other = content.elements[previous];
                if (entry.value != 0 && other.kind == kind && other.group == entry.group && other.value != 0)
                    luaL_error(lua, "radio group has multiple selected elements");
            }
        }
        callback_field(lua, table, "on_change", entry.callback);
    } else if (kind == ElementKind::slider || kind == ElementKind::progress) {
        if (kind == ElementKind::slider) {
            entry.minimum = number_field(lua, table, "min", 0, -1000000, 1000000);
            entry.maximum = number_field(lua, table, "max", 1, -1000000, 1000000);
            if (entry.maximum <= entry.minimum)
                luaL_error(lua, "slider max must exceed min");
            callback_field(lua, table, "on_change", entry.callback);
        }
        field(lua, table, "value");
        entry.value = control_value(lua, luaL_optnumber(lua, -1, entry.minimum), entry);
        lua_pop(lua, 1);
    } else if (kind == ElementKind::textbox) {
        text_field(lua, table, "placeholder", node.placeholder);
        if (!valid_input_text(node.text) || !valid_input_text(node.placeholder))
            luaL_error(lua, "textbox text and placeholder must be valid UTF-8 without NUL");
        node.multiline = boolean_field(lua, table, "multiline", false);
        node.password = boolean_field(lua, table, "password", false);
        node.eye_icon = boolean_field(lua, table, "eye_icon", false);
        callback_field(lua, table, "on_change", entry.callback);
    } else if (kind == ElementKind::scroll) {
        node.scroll_x = boolean_field(lua, table, "scroll_x", false);
        node.scroll_y = boolean_field(lua, table, "scroll_y", true);
        node.scrollbar = boolean_field(lua, table, "scrollbar", true);
    } else if (kind == ElementKind::image) {
        read_image_source(lua, table, node.image);
        entry.source_bytes = node.image.path.size() + node.image.icon.size() + node.image.fallback_icon.size();
        entry.image_bytes = node.image.data.size();
        content.image_bytes += entry.image_bytes;
        if (content.image_bytes > 32 * 1024 * 1024) luaL_error(lua, "content image sources exceed 32 MiB");
        static const char* fits[] = {"stretch", "cover", "contain", "tile", nullptr};
        node.fit = static_cast<ht::eImageFitMode>(enum_field(lua, table, "fit", "contain", fits));
        node.radius = static_cast<int>(integer_field(lua, table, "radius", 0));
        callback_field(lua, table, "on_load", entry.callback);
    } else if (kind == ElementKind::spinbox || kind == ElementKind::combobox) {
        field(lua, table, "items");
        luaL_checktype(lua, -1, LUA_TTABLE);
        const auto count = lua_rawlen(lua, -1);
        if (count == 0 || count > 256)
            luaL_error(lua, "items requires 1..256 strings");
        for (std::size_t item = 1; item <= count; ++item) {
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(item));
            luaL_checktype(lua, -1, LUA_TSTRING);
            std::size_t bytes;
            const char* text = lua_tolstring(lua, -1, &bytes);
            if (bytes > max_text_bytes || std::string_view(text, bytes).contains('\0'))
                luaL_error(lua, "item is too long or contains NUL");
            node.items.emplace_back(text, bytes);
            entry.fixed_bytes += bytes;
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);
        entry.minimum = 1;
        entry.maximum = static_cast<float>(count);
        entry.value = static_cast<float>(integer_field(lua, table, "value", 1, 1, static_cast<lua_Integer>(count)));
        callback_field(lua, table, "on_change", entry.callback);
    } else if (kind == ElementKind::line) {
        node.thickness = static_cast<int>(integer_field(lua, table, "thickness", 1, 1, 256));
        field(lua, table, "points");
        if (lua_isnil(lua, -1))
            node.points = {{0.0, 0.5}, {1.0, 0.5}};
        else {
            luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1);
            if (count < 2 || count > 256)
                luaL_error(lua, "line requires 2..256 points");
            for (std::size_t point = 1; point <= count; ++point) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(point));
                luaL_checktype(lua, -1, LUA_TTABLE);
                const auto x = number_field(lua, -1, "x", 0, 0, 1);
                const auto y = number_field(lua, -1, "y", 0, 0, 1);
                node.points.emplace_back(x, y);
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);
    }
    entry.fixed_bytes += node.font_family.size() + node.placeholder.size() + node.tooltip.size() + entry.id.size() + entry.group.size();
    content.string_bytes += entry.text_bytes + entry.fixed_bytes + entry.source_bytes;
    if (content.string_bytes > max_content_bytes)
        luaL_error(lua, "content strings exceed 1 MiB");
    {
        const auto generation = content.generation;
        const auto sizing = [](int value) {
            return value < 0 ? ht::CDynamicSize::HT_SIZE_PERCENT : value == 0 ? ht::CDynamicSize::HT_SIZE_AUTO : ht::CDynamicSize::HT_SIZE_ABSOLUTE;
        };
        ht::CDynamicSize size(sizing(node.width), sizing(node.height),
                             {node.width < 0 ? 1 : node.width, node.height < 0 ? 1 : node.height});
        CSharedPointer<ht::IElement> element;
        if (entry.kind == ElementKind::row)
            element = ht::CRowLayoutBuilder::begin()->gap(static_cast<std::size_t>(node.gap))->size(std::move(size))->commence();
        else if (entry.kind == ElementKind::column)
            element = ht::CColumnLayoutBuilder::begin()->gap(static_cast<std::size_t>(node.gap))->size(std::move(size))->commence();
        else if (entry.kind == ElementKind::text) {
            auto builder = ht::CTextBuilder::begin()->text(std::move(node.text))->size(std::move(size))
                ->align(node.align)->noEllipsize(!node.ellipsize);
            if (node.color) builder->color([color = *node.color] { return color; });
            if (node.font_size) builder->fontSize({ht::CFontSize::HT_FONT_ABSOLUTE, node.font_size});
            if (!node.font_family.empty()) builder->fontFamily(std::move(node.font_family));
            element = builder->commence();
        } else if (entry.kind == ElementKind::button) {
            auto builder = ht::CButtonBuilder::begin()->label(std::move(node.text))->enabled(node.enabled)->accent(node.accent)
                ->size(std::move(size))->alignText(node.align)->ellipsize(node.ellipsize)->noBg(node.no_background)->noBorder(node.no_border);
            if (node.font_size) builder->fontSize({ht::CFontSize::HT_FONT_ABSOLUTE, node.font_size});
            if (!node.font_family.empty()) builder->fontFamily(std::move(node.font_family));
            element = builder->onMainClick([surface = &surface, generation, index](auto) noexcept {
                    queue_control(*surface, generation, index);
                })->commence();
        } else if (entry.kind == ElementKind::rectangle) {
            auto builder = ht::CRectangleBuilder::begin()->size(std::move(size))->rounding(node.radius)->borderThickness(node.border_width);
            if (node.color) builder->color([color = *node.color] { return color; });
            if (node.border_color) builder->borderColor([color = *node.border_color] { return color; });
            element = builder->commence();
        } else if (entry.kind == ElementKind::checkbox || entry.kind == ElementKind::radio) {
            element = ht::CCheckboxBuilder::begin()->size(std::move(size))->toggled(entry.value != 0)
                ->style(entry.kind == ElementKind::radio ? ht::HT_CHECKBOX_STYLE_RADIO : ht::HT_CHECKBOX_STYLE_CHECKMARK)
                ->onToggled([surface = &surface, generation, index](auto, bool value) noexcept {
                    queue_control(*surface, generation, index, value ? 1.F : 0.F);
                })->commence();
        } else if (entry.kind == ElementKind::slider) {
            element = ht::CSliderBuilder::begin()->size(std::move(size))->min(entry.minimum)->max(entry.maximum)->val(entry.value)
                ->onChanged([surface = &surface, generation, index](auto, float value) noexcept {
                    queue_control(*surface, generation, index, value);
                })->commence();
        } else if (entry.kind == ElementKind::progress) {
            element = ht::CProgressBarBuilder::begin()->size(std::move(size))->value(entry.value)->commence();
        } else if (entry.kind == ElementKind::textbox) {
            element = ht::CTextboxBuilder::begin()->size(std::move(size))->defaultText(std::move(node.text))
                ->placeholder(std::move(node.placeholder))->multiline(node.multiline)->password(node.password)->eyeIcon(node.eye_icon)
                ->onTextEdited([surface = &surface, generation, index](auto, const std::string& text) noexcept {
                    queue_control(*surface, generation, index, 0, text);
                })->commence();
        } else if (entry.kind == ElementKind::scroll) {
            element = ht::CScrollAreaBuilder::begin()->size(std::move(size))->scrollX(node.scroll_x)
                ->scrollY(node.scroll_y)->showScrollbar(node.scrollbar)->commence();
        } else if (entry.kind == ElementKind::image) {
            auto builder = ht::CImageBuilder::begin()->size(std::move(size))->fitMode(node.fit)->rounding(node.radius);
            apply_image_source(*builder, surface.owner, node.image);
            builder->onLoaded([surface = &surface, generation, index](bool loaded) noexcept {
                queue_control(*surface, generation, index, loaded ? 1.F : 0.F, {}, 1);
            });
            element = builder->commence();
        } else if (entry.kind == ElementKind::spinbox) {
            element = ht::CSpinboxBuilder::begin()->size(std::move(size))->label(std::move(node.text))->items(std::move(node.items))
                ->currentItem(static_cast<std::size_t>(entry.value) - 1)->onChanged([surface = &surface, generation, index](auto, std::size_t value) noexcept {
                    queue_control(*surface, generation, index, static_cast<float>(value + 1));
                })->commence();
        } else if (entry.kind == ElementKind::combobox) {
            element = ht::CComboboxBuilder::begin()->size(std::move(size))->items(std::move(node.items))
                ->currentItem(static_cast<std::size_t>(entry.value) - 1)->onChanged([surface = &surface, generation, index](auto, std::size_t value) noexcept {
                    queue_control(*surface, generation, index, static_cast<float>(value + 1));
                })->commence();
        } else if (entry.kind == ElementKind::line) {
            auto builder = ht::CLineBuilder::begin()->size(std::move(size))->thick(node.thickness)->points(std::move(node.points));
            if (node.color) builder->color([color = *node.color] { return color; });
            element = builder->commence();
        } else {
            element = ht::CNullBuilder::begin()->size(std::move(size))->commence();
        }
        element->setMargin(static_cast<float>(node.margin));
        element->setOpacity(node.opacity);
        element->setGrow(node.grow_x, node.grow_y);
        element->setTooltip(std::move(node.tooltip));
        element->setFocusable(entry.focusable && entry.enabled);
        element->setFocusRing(entry.focusable);
        if (node.absolute || node.horizontal || node.vertical) {
            element->setPositionMode(ht::IElement::HT_POSITION_ABSOLUTE);
            element->setAbsolutePosition({node.x, node.y});
        }
        if (node.horizontal)
            element->setPositionFlag(node.horizontal == 1 ? ht::IElement::HT_POSITION_FLAG_LEFT : node.horizontal == 2 ? ht::IElement::HT_POSITION_FLAG_HCENTER : ht::IElement::HT_POSITION_FLAG_RIGHT, true);
        if (node.vertical)
            element->setPositionFlag(node.vertical == 1 ? ht::IElement::HT_POSITION_FLAG_TOP : node.vertical == 2 ? ht::IElement::HT_POSITION_FLAG_VCENTER : ht::IElement::HT_POSITION_FLAG_BOTTOM, true);
        if (parent != SIZE_MAX)
            content.elements[parent].native->addChild(element);
        entry.native = std::move(element);
    }
    if (kind == ElementKind::row || kind == ElementKind::column || kind == ElementKind::rectangle || kind == ElementKind::scroll || kind == ElementKind::button) {
        field(lua, table, "children");
        if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TTABLE);
            const auto count = lua_rawlen(lua, -1);
            if (kind == ElementKind::scroll && count > 1)
                luaL_error(lua, "scroll accepts at most one content child");
            for (std::size_t child = 1; child <= count; ++child) {
                lua_rawgeti(lua, -1, static_cast<lua_Integer>(child));
                build_node(lua, -1, index, depth + 1, surface, content, node);
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);
    }
}

int set_content(lua_State* lua) {
    auto& surface = surface_handle(lua);
    luaL_checktype(lua, 2, LUA_TTABLE);
    if (surface.last_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("surface content generation exhausted");
    auto content = std::make_unique<Content>(lua, ++surface.last_generation, std::vector<Element>{});
    // Keep scratch strings outside pcall; recursive Lua errors skip C++ destructors.
    NodeOptions node;
    lua_pushvalue(lua, 2);
    if (protected_call(lua, [&](lua_State* lua) {
        build_node(lua, 2, SIZE_MAX, 1, surface, *content, node);
        return 0;
    }, 0, 0, 1) < 0)
        return -1;
    if (surface.closed)
        throw std::runtime_error("surface closed while reading content");
    if (surface.window) {
        try {
            surface.window->m_rootElement->addChild(content->elements.front().native);
        } catch (...) {
            surface.window->m_rootElement->removeChild(content->elements.front().native);
            throw;
        }
        if (surface.content)
            surface.window->m_rootElement->removeChild(surface.content->elements.front().native);
    }
    surface.content = std::move(content);
    return 0;
}

Element& named_element(lua_State* lua, Surface& surface) {
    luaL_checktype(lua, 2, LUA_TSTRING);
    std::size_t size;
    const char* id = lua_tolstring(lua, 2, &size);
    if (size == 0 || size > 128)
        luaL_error(lua, "element id must contain 1..128 bytes");
    if (surface.content) {
        for (auto& element : surface.content->elements) {
            if (element.id == std::string_view(id, size))
                return element;
        }
    }
    luaL_error(lua, "unknown element id");
    std::unreachable();
}

int set_children(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& target = named_element(lua, surface);
    if (target.kind != ElementKind::row && target.kind != ElementKind::column && target.kind != ElementKind::rectangle &&
        target.kind != ElementKind::scroll && target.kind != ElementKind::button)
        return luaL_error(lua, "set_children requires a container element");
    luaL_checktype(lua, 3, LUA_TTABLE);
    const auto count = lua_rawlen(lua, 3);
    if (target.kind == ElementKind::scroll && count > 1)
        return luaL_error(lua, "scroll accepts at most one content child");
    const auto revision = surface.content->generation;
    const auto begin = static_cast<std::size_t>(&target - surface.content->elements.data()) + 1;
    auto end = begin;
    const auto depth = target.depth;
    while (end < surface.content->elements.size() && surface.content->elements[end].depth > depth)
        ++end;
    if (surface.last_generation == UINT64_MAX)
        throw std::runtime_error("surface content generation exhausted");
    Content replacement{lua, ++surface.last_generation, {}};
    NodeOptions node;
    lua_pushvalue(lua, 3);
    if (protected_call(lua, [&](lua_State* lua) {
        for (std::size_t child = 1; child <= count; ++child) {
            lua_rawgeti(lua, 2, static_cast<lua_Integer>(child));
            build_node(lua, -1, SIZE_MAX, depth + 1, surface, replacement, node);
            lua_pop(lua, 1);
        }
        return 0;
    }, 0, 0, 1) < 0)
        return -1;
    if (!surface.content || surface.content->generation != revision)
        throw std::runtime_error("surface content changed while reading children");
    auto& content = *surface.content;
    auto bytes = content.string_bytes + replacement.string_bytes;
    auto image_bytes = content.image_bytes + replacement.image_bytes;
    for (auto i = begin; i < end; ++i) image_bytes -= content.elements[i].image_bytes;
    for (auto i = begin; i < end; ++i)
        bytes -= content.elements[i].fixed_bytes + content.elements[i].text_bytes + content.elements[i].source_bytes;
    const auto total = content.elements.size() - (end - begin) + replacement.elements.size();
    if (total > max_nodes || bytes > max_content_bytes || image_bytes > 32 * 1024 * 1024)
        throw std::runtime_error("content exceeds 2048 elements, 1 MiB of strings or 32 MiB of images");
    for (const auto& added : replacement.elements) {
        for (std::size_t i = 0; i < content.elements.size(); ++i) {
            if (i >= begin && i < end)
                continue;
            const auto& retained = content.elements[i];
            if (!added.id.empty() && added.id == retained.id)
                throw std::runtime_error("duplicate element id");
            if (added.kind == ElementKind::radio && added.value != 0 && retained.kind == ElementKind::radio && added.group == retained.group &&
                static_cast<ht::CCheckboxElement*>(retained.native.get())->state())
                throw std::runtime_error("radio group has multiple selected elements");
        }
    }
    std::vector<Element> merged;
    merged.reserve(total);
    const auto parent = content.elements[begin - 1].native;
    // Scroll areas own a private content wrapper; use their public child operations.
    for (auto i = begin; i < end; ++i) {
        if (content.elements[i].depth == depth + 1)
            parent->removeChild(content.elements[i].native);
    }
    try {
        for (const auto& added : replacement.elements) {
            if (added.depth == depth + 1)
                parent->addChild(added.native);
        }
    } catch (...) {
        for (const auto& added : replacement.elements) {
            if (added.depth == depth + 1)
                parent->removeChild(added.native);
        }
        for (auto i = begin; i < end; ++i) {
            if (content.elements[i].depth == depth + 1)
                parent->addChild(content.elements[i].native);
        }
        throw;
    }
    for (std::size_t i = 0; i < begin; ++i)
        merged.push_back(std::move(content.elements[i]));
    for (auto& added : replacement.elements)
        merged.push_back(std::move(added));
    replacement.elements.clear();
    for (auto i = begin; i < end; ++i)
        unref(lua, content.elements[i].callback);
    for (auto i = end; i < content.elements.size(); ++i)
        merged.push_back(std::move(content.elements[i]));
    content.elements = std::move(merged);
    content.string_bytes = bytes;
    content.image_bytes = image_bytes;
    content.generation = replacement.generation;
    return 0;
}

int set_value(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind == ElementKind::checkbox || element.kind == ElementKind::radio) {
        luaL_checktype(lua, 3, LUA_TBOOLEAN);
        static_cast<ht::CCheckboxElement*>(element.native.get())->setState(lua_toboolean(lua, 3));
        if (element.kind == ElementKind::radio && lua_toboolean(lua, 3)) {
            for (auto& other : surface.content->elements) {
                if (&other != &element && other.kind == ElementKind::radio && other.group == element.group)
                    static_cast<ht::CCheckboxElement*>(other.native.get())->setState(false);
            }
        }
        return 0;
    }
    if (element.kind == ElementKind::spinbox || element.kind == ElementKind::combobox) {
        const auto value = luaL_checkinteger(lua, 3);
        control_value(lua, static_cast<lua_Number>(value), element);
        if (element.kind == ElementKind::spinbox)
            static_cast<ht::CSpinboxElement*>(element.native.get())->setCurrent(static_cast<std::size_t>(value - 1));
        else
            static_cast<ht::CComboboxElement*>(element.native.get())->setCurrent(static_cast<std::size_t>(value - 1));
        return 0;
    }
    if (element.kind != ElementKind::slider && element.kind != ElementKind::progress)
        return luaL_error(lua, "set_value requires a value control");
    const auto value = control_value(lua, luaL_checknumber(lua, 3), element);
    if (element.kind == ElementKind::progress)
        static_cast<ht::CProgressBarElement*>(element.native.get())->setValue(value);
    else {
        // Toolkit slider rebuilds emit onChanged even for programmatic updates.
        element.programmatic = true;
        try {
            static_cast<ht::CSliderElement*>(element.native.get())->rebuild()->val(value)->commence();
        } catch (...) {
            element.programmatic = false;
            throw;
        }
        element.programmatic = false;
        element.value = value;
    }
    return 0;
}

int get_value(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind == ElementKind::checkbox || element.kind == ElementKind::radio)
        lua_pushboolean(lua, static_cast<ht::CCheckboxElement*>(element.native.get())->state());
    else if (element.kind == ElementKind::slider)
        lua_pushnumber(lua, element.value);
    else if (element.kind == ElementKind::progress)
        lua_pushnumber(lua, static_cast<ht::CProgressBarElement*>(element.native.get())->value());
    else if (element.kind == ElementKind::spinbox)
        lua_pushinteger(lua, static_cast<lua_Integer>(static_cast<ht::CSpinboxElement*>(element.native.get())->current() + 1));
    else if (element.kind == ElementKind::combobox)
        lua_pushinteger(lua, static_cast<lua_Integer>(static_cast<ht::CComboboxElement*>(element.native.get())->current() + 1));
    else
        return luaL_error(lua, "get_value requires a value control");
    return 1;
}

int set_text(lua_State* lua) {
    luaL_checktype(lua, 3, LUA_TSTRING);
    std::size_t size;
    const char* text = lua_tolstring(lua, 3, &size);
    if (size > max_text_bytes || std::string_view(text, size).contains('\0'))
        return luaL_error(lua, "text is too long or contains NUL");
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    const auto bytes = surface.content->string_bytes - element.text_bytes + size;
    if (bytes > max_content_bytes)
        return luaL_error(lua, "content strings exceed 1 MiB");
    if (element.kind == ElementKind::text)
        static_cast<ht::CTextElement*>(element.native.get())->setText(std::string(text, size));
    else if (element.kind == ElementKind::button)
        static_cast<ht::CButtonElement*>(element.native.get())->setLabel(std::string(text, size));
    else if (element.kind == ElementKind::textbox) {
        if (!valid_input_text(std::string_view(text, size)))
            return luaL_error(lua, "textbox text must be valid UTF-8 without NUL");
        auto* textbox = static_cast<ht::CTextboxElement*>(element.native.get());
        if (textbox->currentText() != std::string_view(text, size)) {
            std::string replacement(text, size);
            try {
                textbox->setText(std::move(replacement));
            } catch (...) {
                const auto retained = textbox->currentText().size();
                surface.content->string_bytes = surface.content->string_bytes - element.text_bytes + retained;
                element.text_bytes = retained;
                throw;
            }
        }
    } else
        return luaL_error(lua, "set_text requires a text, button, or textbox element");
    surface.content->string_bytes = bytes;
    element.text_bytes = size;
    return 0;
}

int get_text(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind != ElementKind::textbox)
        return luaL_error(lua, "get_text requires a textbox element");
    const std::string text(static_cast<ht::CTextboxElement*>(element.native.get())->currentText());
    return protected_call(lua, [&](lua_State* lua) {
        lua_pushlstring(lua, text.data(), text.size());
        return 1;
    }, 1);
}

int set_enabled(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind != ElementKind::button)
        return luaL_error(lua, "set_enabled requires a button element");
    luaL_checktype(lua, 3, LUA_TBOOLEAN);
    static_cast<ht::CButtonElement*>(element.native.get())->setEnabled(lua_toboolean(lua, 3));
    element.enabled = lua_toboolean(lua, 3);
    element.native->setFocusable(element.enabled && element.focusable);
    return 0;
}

int set_image(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind != ElementKind::image)
        return luaL_error(lua, "set_image requires an image element");
    luaL_checktype(lua, 3, LUA_TTABLE);
    const auto generation = surface.content->generation;
    ImageSource source;
    lua_pushvalue(lua, 3);
    if (protected_call(lua, [&](lua_State* lua) {
        read_image_source(lua, 2, source);
        return 0;
    }, 0, 0, 1) < 0)
        return -1;
    if (!surface.content || surface.content->generation != generation)
        throw std::runtime_error("surface content changed while reading image source");
    const auto source_bytes = source.path.size() + source.icon.size() + source.fallback_icon.size();
    const auto bytes = surface.content->string_bytes - element.source_bytes + source_bytes;
    if (bytes > max_content_bytes)
        throw std::runtime_error("content strings exceed 1 MiB");
    const auto image_bytes = surface.content->image_bytes - element.image_bytes + source.data.size();
    if (image_bytes > 32 * 1024 * 1024) throw std::runtime_error("content image sources exceed 32 MiB");
    const auto source_image_bytes = source.data.size();
    auto builder = static_cast<ht::CImageElement*>(element.native.get())->rebuild();
    apply_image_source(*builder, surface.owner, source);
    if (element.source == UINT64_MAX)
        throw std::runtime_error("image source generation exhausted");
    const auto serial = ++element.source;
    builder->onLoaded([surface = &surface, generation = element.generation, index = element.index, serial](bool loaded) noexcept {
        queue_control(*surface, generation, index, loaded ? 1.F : 0.F, {}, serial);
    });
    builder->commence();
    surface.content->string_bytes = bytes;
    element.source_bytes = source_bytes;
    element.image_bytes = source_image_bytes;
    surface.content->image_bytes = image_bytes;
    return 0;
}

int focus(lua_State* lua) {
    if (!lua_isnoneornil(lua, 3))
        luaL_checktype(lua, 3, LUA_TBOOLEAN);
    const bool focused = lua_isnoneornil(lua, 3) || lua_toboolean(lua, 3);
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (focused && (!element.focusable || !element.enabled))
        return luaL_error(lua, "element is not focusable");
    element.native->focusKeyboard(focused);
    return 0;
}

int geometry(lua_State* lua) {
    auto& surface = surface_handle(lua);
    const auto box = named_element(lua, surface).native->geometry();
    return protected_call(lua, [&](lua_State* lua) {
        lua_createtable(lua, 0, 4);
        lua_pushnumber(lua, box.x); lua_setfield(lua, -2, "x");
        lua_pushnumber(lua, box.y); lua_setfield(lua, -2, "y");
        lua_pushnumber(lua, box.w); lua_setfield(lua, -2, "width");
        lua_pushnumber(lua, box.h); lua_setfield(lua, -2, "height");
        return 1;
    }, 1);
}

int set_scroll(lua_State* lua) {
    const auto x = luaL_checknumber(lua, 3), y = luaL_checknumber(lua, 4);
    if (!std::isfinite(x) || !std::isfinite(y))
        return luaL_error(lua, "scroll offsets must be finite");
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind != ElementKind::scroll)
        return luaL_error(lua, "set_scroll requires a scroll element");
    static_cast<ht::CScrollAreaElement*>(element.native.get())->setScroll({x, y});
    return 0;
}

int get_scroll(lua_State* lua) {
    auto& surface = surface_handle(lua);
    auto& element = named_element(lua, surface);
    if (element.kind != ElementKind::scroll)
        return luaL_error(lua, "get_scroll requires a scroll element");
    const auto scroll = static_cast<ht::CScrollAreaElement*>(element.native.get())->getCurrentScroll();
    lua_pushnumber(lua, scroll.x);
    lua_pushnumber(lua, scroll.y);
    return 2;
}

int animate(lua_State* lua) {
    auto& surface = surface_handle(lua);
    const auto& element = named_element(lua, surface);
    static const char* properties[] = {"opacity", "geometry", "color", "border_color", nullptr};
    const int property = luaL_checkoption(lua, 3, nullptr, properties);
    const auto kind = element.kind;
    if ((property == 2 && kind != ElementKind::text && kind != ElementKind::rectangle) || (property == 3 && kind != ElementKind::rectangle))
        return luaL_error(lua, "color animations require text or rectangle elements");
    const auto native = element.native;
    const auto generation = surface.content->generation;
    ht::SAnimation animation = ht::SNoAnimation{};
    if (!lua_isnoneornil(lua, 4)) {
        lua_pushvalue(lua, 4);
        if (protected_call(lua, [&](lua_State* lua) {
            luaL_checktype(lua, 2, LUA_TTABLE);
            static const char* kinds[] = {"none", "bezier", "spring", nullptr};
            const int type = enum_field(lua, 2, "type", "bezier", kinds);
            if (type == 1) {
                ht::SBezierAnimation value;
                value.duration = std::chrono::milliseconds(integer_field(lua, 2, "duration", 200, 1, 60000));
                value.control1 = {number_field(lua, 2, "x1", .23F, 0, 1), number_field(lua, 2, "y1", 1, -10, 10)};
                value.control2 = {number_field(lua, 2, "x2", .32F, 0, 1), number_field(lua, 2, "y2", 1, -10, 10)};
                animation = value;
            } else if (type == 2) {
                ht::SSpringAnimation value;
                value.stiffness = number_field(lua, 2, "stiffness", 250, .01F, 10000);
                value.damping = number_field(lua, 2, "damping", 25, .01F, 1000);
                value.mass = number_field(lua, 2, "mass", 1, .01F, 100);
                animation = value;
            }
            return 0;
        }, 0, 0, 1) < 0) return -1;
    }
    if (surface.closed || !surface.content || surface.content->generation != generation) throw std::runtime_error("content changed while reading animation");
    if (property == 0) native->animateOpacity(animation);
    else if (property == 1) native->animateGeometry(animation);
    else if (property == 3) static_cast<ht::CRectangleElement*>(native.get())->animateBorderColor(animation);
    else if (kind == ElementKind::text) static_cast<ht::CTextElement*>(native.get())->animateColor(animation);
    else static_cast<ht::CRectangleElement*>(native.get())->animateColor(animation);
    return 0;
}

int set_style(lua_State* lua) {
    auto& surface = surface_handle(lua);
    const auto& element = named_element(lua, surface);
    luaL_checktype(lua, 3, LUA_TTABLE);
    const auto kind = element.kind;
    const auto native = element.native;
    const auto generation = surface.content->generation;
    std::optional<ht::CHyprColor> color, border;
    float opacity = -1;
    lua_pushvalue(lua, 3);
    if (protected_call(lua, [&](lua_State* lua) {
        color = color_field(lua, 2, "color"); border = color_field(lua, 2, "border_color");
        opacity = number_field(lua, 2, "opacity", -1, 0, 1); return 0;
    }, 0, 0, 1) < 0) return -1;
    if (surface.closed || !surface.content || surface.content->generation != generation) throw std::runtime_error("content changed while reading style");
    if ((color && kind != ElementKind::text && kind != ElementKind::rectangle) || (border && kind != ElementKind::rectangle))
        throw std::runtime_error("colors require text or rectangle elements");
    if (kind == ElementKind::text && color) static_cast<ht::CTextElement*>(native.get())->rebuild()->color([value = *color] { return value; })->commence();
    if (kind == ElementKind::rectangle && (color || border)) {
        auto builder = static_cast<ht::CRectangleElement*>(native.get())->rebuild();
        if (color) builder->color([value = *color] { return value; });
        if (border) builder->borderColor([value = *border] { return value; });
        builder->commence();
    }
    if (opacity >= 0) native->setOpacity(opacity);
    return 0;
}

}

bool keyboard_input(Surface& surface, const ht::Input::SKeyboardKeyEvent& event) noexcept {
    if (surface.closed || !surface.visible)
        return false;
    try {
        const auto modifiers = event.modMask & (ht::Input::HT_MODIFIER_SHIFT | ht::Input::HT_MODIFIER_CTRL | ht::Input::HT_MODIFIER_ALT | ht::Input::HT_MODIFIER_META);
        for (std::size_t i = 0; i < surface.shortcuts.size(); ++i) {
            const auto& shortcut = surface.shortcuts[i];
            if (xkb_keysym_to_lower(event.xkbKeysym) != shortcut.key || modifiers != shortcut.modifiers)
                continue;
            if (event.down && !event.repeat) {
                auto& gui = surface.owner;
                if (gui.event_count == gui.events.size())
                    gui.gui_error = "GUI control event queue limit reached";
                else {
                    auto& queued = gui.events[gui.event_count++];
                    queued.surface = &surface;
                    queued.generation = 0;
                    queued.element = i;
                    queued.epoch = surface.epoch;
                }
                wake(gui);
            }
            return true;
        }
        if (surface.kind == ht::HT_WINDOW_POPUP && event.xkbKeysym == XKB_KEY_Escape) {
            if (event.down) {
                surface.close_pending = true;
                wake(surface.owner);
            }
            return true;
        }
        if (!surface.content)
            return false;
        auto& elements = surface.content->elements;
        std::size_t focused = elements.size();
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (elements[i].native->keyboardFocused()) { focused = i; break; }
        }
        if ((event.xkbKeysym == XKB_KEY_Tab || event.xkbKeysym == XKB_KEY_ISO_Left_Tab) && !(modifiers & ~static_cast<std::uint32_t>(ht::Input::HT_MODIFIER_SHIFT))) {
            if (!event.down)
                return true;
            const bool backwards = (modifiers & ht::Input::HT_MODIFIER_SHIFT) || event.xkbKeysym == XKB_KEY_ISO_Left_Tab;
            auto index = focused == elements.size() ? (backwards ? 0 : elements.size() - 1) : focused;
            for (std::size_t step = 0; step < elements.size(); ++step) {
                index = backwards ? (index + elements.size() - 1) % elements.size() : (index + 1) % elements.size();
                if (elements[index].focusable && elements[index].enabled) {
                    elements[index].native->focusKeyboard();
                    break;
                }
            }
            return true;
        }
        if (focused == elements.size() || modifiers)
            return false;
        auto& element = elements[focused];
        if (!element.enabled || element.kind == ElementKind::textbox)
            return false;
        const bool activate = event.xkbKeysym == XKB_KEY_Return || event.xkbKeysym == XKB_KEY_space;
        const int direction = event.xkbKeysym == XKB_KEY_Left || event.xkbKeysym == XKB_KEY_Down ? -1 :
                              event.xkbKeysym == XKB_KEY_Right || event.xkbKeysym == XKB_KEY_Up ? 1 : 0;
        if (activate && (element.kind == ElementKind::button || element.kind == ElementKind::checkbox || element.kind == ElementKind::radio)) {
            if (event.down && !event.repeat) {
                float value = 0;
                if (element.kind != ElementKind::button) {
                    auto* checkbox = static_cast<ht::CCheckboxElement*>(element.native.get());
                    if (element.kind == ElementKind::radio && checkbox->state())
                        return true;
                    value = element.kind == ElementKind::radio || !checkbox->state() ? 1.F : 0.F;
                    checkbox->setState(value != 0);
                }
                queue_control(surface, element.generation, element.index, value);
            }
            return true;
        }
        if (direction && (element.kind == ElementKind::slider || element.kind == ElementKind::spinbox || element.kind == ElementKind::combobox)) {
            if (!event.down)
                return true;
            float value;
            if (element.kind == ElementKind::slider) {
                value = std::clamp(element.value + static_cast<float>(direction) * (element.maximum - element.minimum) / 100.F, element.minimum, element.maximum);
                static_cast<ht::CSliderElement*>(element.native.get())->rebuild()->val(value)->commence();
            } else {
                const auto current = element.kind == ElementKind::spinbox ? static_cast<ht::CSpinboxElement*>(element.native.get())->current() :
                    static_cast<ht::CComboboxElement*>(element.native.get())->current();
                value = std::clamp(static_cast<float>(current + 1) + static_cast<float>(direction), element.minimum, element.maximum);
                if (element.kind == ElementKind::spinbox)
                    static_cast<ht::CSpinboxElement*>(element.native.get())->setCurrent(static_cast<std::size_t>(value) - 1);
                else
                    static_cast<ht::CComboboxElement*>(element.native.get())->setCurrent(static_cast<std::size_t>(value) - 1);
                if (value != static_cast<float>(current + 1))
                    queue_control(surface, element.generation, element.index, value);
            }
            return true;
        }
    } catch (...) {
        surface.owner.gui_error = "cannot process keyboard input";
        wake(surface.owner);
    }
    return false;
}

void register_content(lua_State* lua) {
    const luaL_Reg methods[] = {
        {"set_content", guarded<set_content>}, {"set_children", guarded<set_children>},
        {"set_value", guarded<set_value>}, {"get_value", guarded<get_value>},
        {"set_text", guarded<set_text>}, {"get_text", guarded<get_text>},
        {"focus", guarded<focus>}, {"set_image", guarded<set_image>},
        {"set_enabled", guarded<set_enabled>}, {"geometry", guarded<geometry>},
        {"animate", guarded<animate>}, {"set_style", guarded<set_style>},
        {"set_scroll", guarded<set_scroll>}, {"get_scroll", guarded<get_scroll>},
        {nullptr, nullptr}
    };
    luaL_setfuncs(lua, methods, 0);
}

}
