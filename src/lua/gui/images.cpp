#include "images.hpp"
#include <hyprworks/host.hpp>
#include <librsvg/rsvg.h>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

namespace hyprworks::lua {
namespace {

constexpr std::size_t input_limit = 8 * 1024 * 1024;
constexpr std::size_t image_limit = 128 * 1024 * 1024;

void reserve(Image& image, std::size_t bytes) {
    auto used = image.work.bytes.load(std::memory_order_relaxed);
    do {
        if (bytes > image_limit - used) throw std::runtime_error("image memory limit reached (128 MiB)");
    } while (!image.work.bytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    image.bytes += bytes;
}

void allocate(Image& image) {
    if (image.width < 1 || image.height < 1 || image.width > 4096 || image.height > 4096)
        throw std::runtime_error("image dimensions must be in 1..4096");
    reserve(image, static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
    image.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, image.width, image.height);
    if (cairo_surface_status(image.surface) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error(cairo_status_to_string(cairo_surface_status(image.surface)));
}

void premultiply(Image& image, const unsigned char* source, int channels) {
    auto* target = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(image.surface));
    const auto count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t r = source[0], g = source[1], b = source[2], a = channels == 4 ? source[3] : 255;
        target[i] = a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | (b * a + 127) / 255;
        source += channels;
    }
    cairo_surface_mark_dirty(image.surface);
}

void decode(Image& image) {
    std::string resolved;
    if (!image.icon.empty()) {
        resolved = icon_path(image.work.icons, image.theme, image.icon, image.pixels);
        if (resolved.empty()) throw std::runtime_error("icon not found: " + image.icon);
    }
    const auto& path = image.icon.empty() ? image.path : resolved;
    if (!path.empty()) {
        reserve(image, input_limit);
        auto data = host::read_file(path, input_limit);
        if (!data) throw std::runtime_error(data.error());
        image.encoded = std::move(*data);
    }
    if (image.channels) {
        allocate(image);
        const auto required = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * static_cast<std::size_t>(image.channels);
        if (image.encoded.size() != required) throw std::runtime_error("raw image byte count does not match its dimensions");
        premultiply(image, reinterpret_cast<const unsigned char*>(image.encoded.data()), image.channels);
    } else if (image.encoded.size() >= 8 && png_sig_cmp(reinterpret_cast<const unsigned char*>(image.encoded.data()), 0, 8) == 0) {
        png_image png{};
        png.version = PNG_IMAGE_VERSION;
        struct Cleanup { png_image& png; ~Cleanup() { png_image_free(&png); } } cleanup{png};
        if (!png_image_begin_read_from_memory(&png, image.encoded.data(), image.encoded.size()))
            throw std::runtime_error(png.message);
        if (png.width > 4096 || png.height > 4096) throw std::runtime_error("PNG dimensions exceed 4096");
        image.width = static_cast<int>(png.width); image.height = static_cast<int>(png.height);
        allocate(image);
        png.format = PNG_FORMAT_RGBA;
        auto* pixels = cairo_image_surface_get_data(image.surface);
        if (!png_image_finish_read(&png, nullptr, pixels, cairo_image_surface_get_stride(image.surface), nullptr))
            throw std::runtime_error(png.message);
        premultiply(image, pixels, 4);
    } else {
        GError* error = nullptr;
        const std::unique_ptr<RsvgHandle, decltype(&g_object_unref)> svg(
            rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(image.encoded.data()), image.encoded.size(), &error), g_object_unref);
        if (!svg) {
            std::string message = error ? error->message : "expected PNG or SVG";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        double width = 0, height = 0;
        if (!rsvg_handle_get_intrinsic_size_in_pixels(svg.get(), &width, &height)) width = height = 64;
        if (image.pixels > 0) {
            const double ratio = width > 0 && height > 0 ? width / height : 1;
            width = image.pixels; height = width / ratio;
        }
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || width > 4096 || height > 4096)
            throw std::runtime_error("SVG dimensions must be in 1..4096");
        image.width = static_cast<int>(std::ceil(width)); image.height = static_cast<int>(std::ceil(height));
        allocate(image);
        const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(cairo_create(image.surface), cairo_destroy);
        const RsvgRectangle viewport{0, 0, static_cast<double>(image.width), static_cast<double>(image.height)};
        if (!rsvg_handle_render_document(svg.get(), cr.get(), &viewport, &error)) {
            std::string message = error ? error->message : "SVG render failed";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
    }
}

}

void Image::reserve_input(std::size_t size) {
    if (size > input_limit) throw std::runtime_error("image input exceeds 8 MiB");
    reserve(*this, size);
}

Image::~Image() {
    if (surface) cairo_surface_destroy(surface);
    work.bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

bool ImageWork::submit(const std::shared_ptr<Image>& image) {
    std::lock_guard lock(mutex);
    std::erase_if(queue, [](const auto& job) { return job.use_count() == 1; });
    if (stopping || queue.size() >= 32) return false;
    if (image->encoded.size() > input_limit) throw std::runtime_error("image input exceeds 8 MiB");
    if (!image->bytes) image->reserve_input(image->encoded.size());
    queue.push_back(image);
    image->submitted = true;
    changed.notify_one();
    return true;
}

void ImageWork::start(int wake) {
    wake_fd = wake;
    thread = std::thread([this] {
        for (;;) {
            std::shared_ptr<Image> image;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                image = std::move(queue.front()); queue.pop_front();
            }
            if (image.use_count() > 1) {
                try { decode(*image); }
                catch (const std::exception& error) { image->error = error.what(); }
                catch (...) { image->error = "image decoding failed"; }
                if (!image->error.empty() && image->surface) {
                    cairo_surface_destroy(image->surface); image->surface = nullptr;
                }
                std::string{}.swap(image->encoded);
                const auto pixels = image->surface ? static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height) * 4 : 0;
                bytes.fetch_sub(image->bytes - pixels, std::memory_order_relaxed);
                image->bytes = pixels;
                image->ready.store(true, std::memory_order_release);
            }
            image.reset();
            const std::uint64_t value = 1;
            while (::write(wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
        }
    });
}

void ImageWork::stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        queue.clear();
    }
    changed.notify_one();
    if (thread.joinable()) thread.join();
}

ImageWork::~ImageWork() { stop(); }

}
