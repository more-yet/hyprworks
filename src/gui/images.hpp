#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <cairo.h>

namespace hyprworks::lua {

struct Images;
struct Image {
    Images& images;
    std::string path, encoded, icon, theme, error;
    int width = 0, height = 0, channels = 0, pixels = 0;
    int icon_size = 0;
    cairo_surface_t* surface = nullptr;
    std::size_t bytes = 0;
    bool submitted = false;
    std::atomic<bool> ready{false};
    explicit Image(Images& owner) : images(owner) {}
    void allocate();
    void set_data(std::string_view data);
    ~Image();
};

struct Images {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Image>> queue;
    std::atomic<std::size_t> bytes{0};
    std::thread decode_thread;
    bool stopping = false;
    void start(int wake);
    void stop();
    bool submit(const std::shared_ptr<Image>& image);
    ~Images();
};

}
