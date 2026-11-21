#include "gui.hpp"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace hyprworks::lua {

namespace {

constexpr std::size_t max_buffer_bytes = 256 * 1024 * 1024;

Box intersect(Box a, Box b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.w, b.x + b.w) - x),
        std::max(0.0, std::min(a.y + a.h, b.y + b.h) - y)};
}

void text_layout(Gui& gui, Element& element, double width) {
    if (!element.layout) {
        element.layout = pango_layout_new(gui.text_context);
        auto* font = pango_font_description_from_string(element.font.c_str());
        pango_layout_set_font_description(element.layout, font); pango_font_description_free(font);
    }
    std::string display;
    if (element.kind == Kind::input && element.text.empty() && element.preedit.empty()) {
        pango_layout_set_text(element.layout, element.placeholder.data(), static_cast<int>(element.placeholder.size()));
    } else if (element.password) {
        display.assign(static_cast<std::size_t>(g_utf8_strlen(element.text.c_str(), -1)), '*');
        pango_layout_set_text(element.layout, display.data(), static_cast<int>(display.size()));
    } else if (element.kind == Kind::input && !element.preedit.empty()) {
        display = element.text;
        display.insert(static_cast<std::size_t>(element.cursor), element.preedit);
        pango_layout_set_text(element.layout, display.data(), static_cast<int>(display.size()));
    } else pango_layout_set_text(element.layout, element.text.data(), static_cast<int>(element.text.size()));
    pango_layout_set_single_paragraph_mode(element.layout, element.kind == Kind::input);
    pango_layout_set_wrap(element.layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(element.layout, static_cast<PangoAlignment>(element.align));
    pango_layout_set_ellipsize(element.layout, element.wrap || element.kind == Kind::input ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
    pango_layout_set_width(element.layout, width < 0 || element.kind == Kind::input ? -1 :
        static_cast<int>(std::clamp(width, 0.0, 16384.0) * PANGO_SCALE));
}

void measure(Surface& surface, int index, double width) {
    auto& elements = surface.elements;
    auto& element = elements[static_cast<std::size_t>(index)];
    element.box.w = std::max(0.0, width);
    const double inner = std::max(0.0, width - element.padding * 2);
    double height = 0;
    if (element.layout) {
        text_layout(*surface.gui, element, inner);
        int w, h; pango_layout_get_pixel_size(element.layout, &w, &h);
        height = h;
    } else if (element.kind == Kind::image) height = element.natural_h;
    else if (element.kind == Kind::slider) height = 24;
    double used = 0, growth = 0; int count = 0;
    const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
    const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        const auto& item = elements[static_cast<std::size_t>(child)];
        if (element.kind == Kind::row) {
            if (item.grow > 0) growth += item.grow;
            else used += item.natural_w;
        }
        ++count;
    }
    const double gaps = element.gap * std::max(0, count - 1);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        auto& item = elements[static_cast<std::size_t>(child)];
        double child_width = item.width >= 0 ? item.width : item.halign ? item.natural_w : inner;
        if (element.kind == Kind::row)
            child_width = item.grow > 0 && growth > 0 ? std::max(0.0, inner - used - gaps) * item.grow / growth : item.natural_w;
        else if (horizontal) child_width = item.natural_w;
        measure(surface, child, child_width);
        if (vertical) height += item.natural_h;
        else height = std::max(height, item.natural_h);
    }
    if (vertical) height += gaps;
    element.natural_h = element.height >= 0 ? element.height : height + 2 * element.padding;
    if (element.kind == Kind::scroll && element.height < 0) element.natural_h = std::min(element.natural_h, 240.0);
}

void arrange(Surface& surface, int index, Box box, Box clip) {
    auto& elements = surface.elements;
    auto& element = elements[static_cast<std::size_t>(index)];
    element.box = box; element.clip = intersect(box, clip);
    const Box inner{box.x + element.padding, box.y + element.padding,
        std::max(0.0, box.w - element.padding * 2), std::max(0.0, box.h - element.padding * 2)};
    double used = 0, growth = 0; int count = 0;
    const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
    const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        const auto& item = elements[static_cast<std::size_t>(child)];
        if (item.grow > 0 && element.kind == Kind::column) growth += item.grow;
        else used += horizontal ? item.box.w : item.natural_h;
        ++count;
    }
    used += element.gap * std::max(0, count - 1);
    if (element.kind == Kind::scroll) {
        element.scroll_limit = std::max(0.0, used - (horizontal ? inner.w : inner.h));
        element.scroll = std::clamp(element.scroll, 0.0, element.scroll_limit);
    }
    double offset = element.kind == Kind::scroll ? -element.scroll : 0;
    for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
        auto& item = elements[static_cast<std::size_t>(child)];
        Box child_box{inner.x, inner.y, item.box.w, item.height >= 0 ? item.height : item.valign ? item.natural_h : inner.h};
        if (horizontal) { child_box.x += offset; offset += child_box.w + element.gap; }
        else if (vertical) {
            child_box.y += offset;
            child_box.h = item.grow > 0 && growth > 0 ? std::max(0.0, inner.h - used) * item.grow / growth : item.natural_h;
            offset += child_box.h + element.gap;
        }
        if (!horizontal && item.halign >= 2) child_box.x += (inner.w - child_box.w) * (item.halign == 2 ? 0.5 : 1);
        if (!vertical && item.valign >= 2) child_box.y += (inner.h - child_box.h) * (item.valign == 2 ? 0.5 : 1);
        child_box.x += item.x; child_box.y += item.y;
        arrange(surface, child, child_box, intersect(inner, clip));
    }
}

void color(cairo_t* cr, std::uint32_t rgba) {
    cairo_set_source_rgba(cr, (rgba >> 24) / 255.0, ((rgba >> 16) & 255) / 255.0, ((rgba >> 8) & 255) / 255.0, (rgba & 255) / 255.0);
}

void rounded(cairo_t* cr, Box box, double radius) {
    const double r = std::min({radius, box.w / 2, box.h / 2});
    constexpr double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, box.x + box.w - r, box.y + r, r, -pi / 2, 0);
    cairo_arc(cr, box.x + box.w - r, box.y + box.h - r, r, 0, pi / 2);
    cairo_arc(cr, box.x + r, box.y + box.h - r, r, pi / 2, pi);
    cairo_arc(cr, box.x + r, box.y + r, r, pi, pi * 1.5);
    cairo_close_path(cr);
}

void paint(Surface& surface, cairo_t* cr) {
    for (std::size_t index = 0; index < surface.elements.size(); ++index) {
        auto& element = surface.elements[index];
        const auto box = element.box;
        if (element.clip.w <= 0 || element.clip.h <= 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, element.clip.x, element.clip.y, element.clip.w, element.clip.h); cairo_clip(cr);
        color(cr, element.kind == Kind::toggle && element.value != 0 ? element.accent : element.background);
        rounded(cr, box, element.radius); cairo_fill(cr);
        if (element.border_width > 0) {
            const double inset = element.border_width / 2;
            color(cr, element.border_color); cairo_set_line_width(cr, element.border_width);
            rounded(cr, {box.x + inset, box.y + inset, std::max(0.0, box.w - 2 * inset),
                std::max(0.0, box.h - 2 * inset)}, element.radius);
            cairo_stroke(cr);
        }
        if (element.kind == Kind::slider) {
            color(cr, element.accent);
            rounded(cr, {box.x + 4, box.y + box.h / 2 - 2, std::max(0.0, box.w - 8) * element.value, 4}, 2); cairo_fill(cr);
            cairo_arc(cr, box.x + 4 + std::max(0.0, box.w - 8) * element.value, box.y + box.h / 2, 5, 0, 6.283185307); cairo_fill(cr);
        }
        if (element.kind == Kind::image && element.image && element.image->ready.load(std::memory_order_acquire) && element.image->error.empty()) {
            const auto& image = *element.image;
            const double factor = std::min(box.w / image.width, box.h / image.height);
            cairo_save(cr);
            cairo_translate(cr, box.x + (box.w - image.width * factor) / 2, box.y + (box.h - image.height * factor) / 2);
            if (factor > 0) {
                cairo_scale(cr, factor, factor); cairo_set_source_surface(cr, image.surface, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR); cairo_paint(cr);
            }
            cairo_restore(cr);
        }
        if (element.layout) {
            const double x = box.x + element.padding, y = box.y + element.padding;
            cairo_rectangle(cr, x, y, std::max(0.0, box.w - element.padding * 2), std::max(0.0, box.h - element.padding * 2));
            cairo_clip(cr);
            double offset = 0;
            PangoRectangle cursor{};
            if (element.kind == Kind::input) {
                cursor = input_cursor(element, static_cast<int>(index) == surface.focused);
                offset = element.scroll;
                if ((element.cursor != element.anchor && element.preedit.empty()) ||
                    (!element.preedit.empty() && element.preedit_cursor >= 0 && element.preedit_end >= 0 && element.preedit_cursor != element.preedit_end)) {
                    const int begin = element.preedit.empty() ? std::min(element.cursor, element.anchor) : element.cursor + std::min(element.preedit_cursor, element.preedit_end);
                    const int end = element.preedit.empty() ? std::max(element.cursor, element.anchor) : element.cursor + std::max(element.preedit_cursor, element.preedit_end);
                    int* ranges = nullptr, count = 0;
                    auto* line = pango_layout_get_line_readonly(element.layout, 0);
                    pango_layout_line_get_x_ranges(line, layout_index(element, begin), layout_index(element, end), &ranges, &count);
                    color(cr, (element.accent & 0xffffff00) | 100);
                    for (int i = 0; i < count; ++i)
                        cairo_rectangle(cr, x + ranges[i * 2] / double(PANGO_SCALE) - offset, y,
                            (ranges[i * 2 + 1] - ranges[i * 2]) / double(PANGO_SCALE), cursor.height / double(PANGO_SCALE));
                    cairo_fill(cr); g_free(ranges);
                }
            }
            color(cr, element.kind == Kind::input && element.text.empty() && element.preedit.empty() ?
                (element.color & 0xffffff00) | ((element.color & 255) / 2) : element.color);
            cairo_move_to(cr, x - offset, y); pango_cairo_show_layout(cr, element.layout);
            if (element.kind == Kind::input && static_cast<int>(index) == surface.focused) {
                if (element.preedit.empty() || element.preedit_cursor >= 0) {
                    cairo_rectangle(cr, x + cursor.x / double(PANGO_SCALE) - offset, y + cursor.y / double(PANGO_SCALE),
                        1, cursor.height / double(PANGO_SCALE)); cairo_fill(cr);
                }
                if (!element.preedit.empty()) {
                    PangoRectangle start{}, end{};
                    pango_layout_get_cursor_pos(element.layout, element.cursor, &start, nullptr);
                    pango_layout_get_cursor_pos(element.layout, element.cursor + static_cast<int>(element.preedit.size()), &end, nullptr);
                    cairo_rectangle(cr, x + std::min(start.x, end.x) / double(PANGO_SCALE) - offset,
                        y + cursor.height / double(PANGO_SCALE) - 1, std::abs(end.x - start.x) / double(PANGO_SCALE), 1); cairo_fill(cr);
                }
            }
        }
        cairo_restore(cr);
        if (surface.focus_visible && static_cast<int>(index) == surface.focused && focusable(element)) {
            cairo_save(cr);
            cairo_rectangle(cr, element.clip.x, element.clip.y, element.clip.w, element.clip.h); cairo_clip(cr);
            color(cr, element.accent); cairo_set_line_width(cr, 1);
            rounded(cr, {box.x + 0.5, box.y + 0.5, std::max(0.0, box.w - 1), std::max(0.0, box.h - 1)}, element.radius);
            cairo_stroke(cr); cairo_restore(cr);
        }
    }
}

const wl_buffer_listener buffer_listener{
    .release = [](void* data, wl_buffer*) {
        auto& buffer = *static_cast<Buffer*>(data); buffer.busy = false;
        for (auto& surface : buffer.gui->surfaces) if (surface) surface->waiting_buffer = false;
    }
};
const wl_callback_listener frame_listener{
    .done = [](void* data, wl_callback* callback, std::uint32_t) {
        auto& surface = *static_cast<Surface*>(data);
        wl_callback_destroy(callback); surface.frame = nullptr;
    }
};

}

void layout(Surface& surface) {
    if (!surface.layout_dirty || surface.elements.empty()) return;
    auto& elements = surface.elements;
    for (int index = static_cast<int>(elements.size()) - 1; index >= 0; --index) {
        auto& element = elements[static_cast<std::size_t>(index)];
        double width = 0, height = 0; int count = 0;
        const bool horizontal = element.kind == Kind::row || (element.kind == Kind::scroll && element.horizontal);
        const bool vertical = element.kind == Kind::column || (element.kind == Kind::scroll && !element.horizontal);
        if (element.kind == Kind::text || element.kind == Kind::button || element.kind == Kind::toggle || element.kind == Kind::input) {
            text_layout(*surface.gui, element, -1);
            int w, h; pango_layout_get_pixel_size(element.layout, &w, &h); width = w; height = h;
            if (element.kind == Kind::input) width = std::max(width, 160.0);
        } else if (element.kind == Kind::image) {
            width = height = 32;
            if (element.image && element.image->ready.load(std::memory_order_acquire) && element.image->error.empty()) {
                width = element.image->width / surface.scale; height = element.image->height / surface.scale;
            }
        } else if (element.kind == Kind::slider) { width = 120; height = 24; }
        for (int child = index + 1; child < element.end; child = elements[static_cast<std::size_t>(child)].end) {
            const auto& item = elements[static_cast<std::size_t>(child)];
            if (horizontal) width += item.natural_w; else width = std::max(width, item.natural_w);
            if (vertical) height += item.natural_h; else height = std::max(height, item.natural_h);
            ++count;
        }
        if (horizontal) width += std::max(0, count - 1) * element.gap;
        if (vertical) height += std::max(0, count - 1) * element.gap;
        element.natural_w = element.width >= 0 ? element.width : width + element.padding * 2;
        element.natural_h = element.height >= 0 ? element.height : height + element.padding * 2;
    }
    measure(surface, 0, surface.width);
    arrange(surface, 0, {0, 0, static_cast<double>(surface.width), static_cast<double>(surface.height)},
        {0, 0, static_cast<double>(surface.width), static_cast<double>(surface.height)});
    surface.layout_dirty = false;
}

void reveal_focus(Surface& surface, bool native) {
    layout(surface);
    auto& elements = surface.elements;
    const auto& focused = elements[static_cast<std::size_t>(surface.focused)];
    auto target = focused.box;
    bool changed = false;
    for (int index = focused.parent; index >= 0; index = elements[static_cast<std::size_t>(index)].parent) {
        auto& element = elements[static_cast<std::size_t>(index)];
        const Box viewport{element.box.x + element.padding, element.box.y + element.padding,
            std::max(0.0, element.box.w - element.padding * 2), std::max(0.0, element.box.h - element.padding * 2)};
        if (element.kind == Kind::scroll) {
            const double start = element.horizontal ? target.x : target.y;
            const double size = element.horizontal ? target.w : target.h;
            const double visible_start = element.horizontal ? viewport.x : viewport.y;
            const double visible_size = element.horizontal ? viewport.w : viewport.h;
            const double end = start + size, visible_end = visible_start + visible_size;
            double delta = 0;
            if (size <= visible_size) {
                if (start < visible_start) delta = start - visible_start;
                else if (end > visible_end) delta = end - visible_end;
            } else {
                if (start > visible_start) delta = start - visible_start;
                else if (end < visible_end) delta = end - visible_end;
            }
            const double next = std::clamp(element.scroll + delta, 0.0, element.scroll_limit);
            if (next != element.scroll) {
                if (native) {
                    if (!element.scroll_origin) element.scroll_origin = element.scroll;
                } else element.scroll_origin.reset();
                if (element.horizontal) target.x -= next - element.scroll;
                else target.y -= next - element.scroll;
                element.scroll = next;
                changed = true;
            }
        }
        target = intersect(target, viewport);
    }
    if (changed) {
        arrange(surface, 0, elements.front().box, elements.front().box);
        surface.dirty = true;
    }
}

int layout_index(const Element& element, int index) {
    return element.password ? static_cast<int>(g_utf8_pointer_to_offset(element.text.c_str(), element.text.c_str() + index)) : index;
}

PangoRectangle input_cursor(Element& element, bool focused) {
    const int position = element.cursor + (element.preedit_cursor >= 0 ? std::min(element.preedit_cursor, static_cast<int>(element.preedit.size())) : 0);
    PangoRectangle cursor{};
    pango_layout_get_cursor_pos(element.layout, layout_index(element, position), &cursor, nullptr);
    if (focused) {
        const double x = std::max(0.0, cursor.x / double(PANGO_SCALE));
        const double visible = std::max(0.0, element.box.w - element.padding * 2 - 2);
        element.scroll = std::clamp(element.scroll, std::max(0.0, x - visible), x);
    }
    return cursor;
}

void render(Surface& surface) {
    if (!surface.configured || surface.closed || surface.close_pending || !surface.dirty || surface.frame) return;
    auto& gui = *surface.gui;
    const auto width = static_cast<int>(std::ceil(surface.width * surface.scale));
    const auto height = static_cast<int>(std::ceil(surface.height * surface.scale));
    if (width < 1 || height < 1 || width > 16384 || height > 16384) throw std::runtime_error("scaled surface dimensions exceed 16384");
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (bytes > max_buffer_bytes / 2) throw std::runtime_error("surface exceeds 128 MiB buffer limit");
    Buffer* buffer = nullptr;
    std::size_t count = 0;
    std::erase_if(gui.buffers, [&](auto& item) {
        const auto* owner = find_surface(gui, item->surface);
        return !item->busy && (!owner || (item->surface == surface.id && (item->width != width || item->height != height)));
    });
    for (auto& item : gui.buffers) {
        if (item->surface != surface.id) continue;
        ++count;
        if (!item->busy && item->width == width && item->height == height) buffer = item.get();
    }
    if (!buffer) {
        if (count >= 3) { surface.waiting_buffer = true; return; }
        if (bytes > max_buffer_bytes - gui.buffer_bytes) {
            std::erase_if(gui.buffers, [](const auto& item) { return !item->busy; });
            if (bytes > max_buffer_bytes - gui.buffer_bytes) { surface.waiting_buffer = true; return; }
        }
        auto replacement = std::make_unique<Buffer>();
        replacement->gui = &gui; replacement->surface = surface.id;
        replacement->width = width; replacement->height = height;
        const int fd = memfd_create("hyprworks-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) throw std::runtime_error("cannot create presentation buffer");
        if (ftruncate(fd, static_cast<off_t>(bytes)) < 0) { close(fd); throw std::runtime_error("cannot size presentation buffer"); }
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
        replacement->mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (replacement->mapping == MAP_FAILED) { close(fd); throw std::runtime_error("cannot map presentation buffer"); }
        replacement->bytes = bytes; gui.buffer_bytes += bytes;
        auto* pool = wl_shm_create_pool(gui.shm, fd, static_cast<int>(bytes));
        close(fd);
        if (!pool) throw std::runtime_error("cannot create Wayland buffer pool");
        replacement->native = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (!replacement->native) throw std::runtime_error("cannot create Wayland buffer");
        wl_buffer_add_listener(replacement->native, &buffer_listener, replacement.get());
        replacement->cairo = cairo_image_surface_create_for_data(static_cast<unsigned char*>(replacement->mapping), CAIRO_FORMAT_ARGB32, width, height, width * 4);
        if (cairo_surface_status(replacement->cairo) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("cannot create Cairo buffer");
        buffer = replacement.get(); gui.buffers.push_back(std::move(replacement));
    }
    layout(surface);
    const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(cairo_create(buffer->cairo), cairo_destroy);
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr.get(), 0, 0, 0, 0); cairo_paint(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_OVER);
    cairo_scale(cr.get(), static_cast<double>(width) / surface.width, static_cast<double>(height) / surface.height);
    paint(surface, cr.get());
    if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(cairo_status(cr.get())));
    cairo_surface_flush(buffer->cairo);
    if (surface.viewport) {
        wl_surface_set_buffer_scale(surface.native, 1);
        wp_viewport_set_destination(surface.viewport, surface.width, surface.height);
    } else wl_surface_set_buffer_scale(surface.native, static_cast<int>(surface.scale));
    if (surface.xdg) xdg_surface_set_window_geometry(surface.xdg, 0, 0, surface.width, surface.height);
    surface.frame = wl_surface_frame(surface.native);
    if (!surface.frame) throw std::runtime_error("cannot create frame callback");
    wl_callback_add_listener(surface.frame, &frame_listener, &surface);
    wl_surface_attach(surface.native, buffer->native, 0, 0);
    wl_surface_damage_buffer(surface.native, 0, 0, width, height);
    buffer->busy = true; surface.dirty = false;
    wl_surface_commit(surface.native);
    surface.mapped = true;
    surface.waiting_buffer = false;
}

Buffer::~Buffer() {
    if (native) wl_buffer_destroy(native);
    if (cairo) cairo_surface_destroy(cairo);
    if (mapping && mapping != MAP_FAILED) munmap(mapping, bytes);
    gui->buffer_bytes -= bytes;
}

}
