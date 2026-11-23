#pragma once

#include "icons.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <cairo.h>

namespace hyprworks::lua {

struct ImageWork;
/// The worker mutates a submitted image until release-publishing ready.
/// Readers acquire ready before accessing pixels; ImageWork outlives all images.
struct Image {
    ImageWork& work;
    std::string path, encoded, icon, theme, error;
    int width = 0, height = 0, channels = 0, pixels = 0;
    int icon_size = 0;
    cairo_surface_t* surface = nullptr;
    std::size_t bytes = 0;
    bool submitted = false;
    std::atomic<bool> ready{false};
    explicit Image(ImageWork& owner) : work(owner) {}
    void allocate();
    void set_data(std::string_view data);
    ~Image();
};

struct ImageWork {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Image>> queue;
    std::atomic<std::size_t> bytes{0};
    std::thread thread;
    bool stopping = false;
    int wake_fd = -1;
    std::optional<IconTheme> icons;
    void start(int wake);
    void stop();
    bool submit(const std::shared_ptr<Image>& image);
    ~ImageWork();
};

}
