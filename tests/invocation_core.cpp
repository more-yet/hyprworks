#include <hyprworks/lua.hpp>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace hyprworks::lua;

int main() {
    auto path = (std::filesystem::temp_directory_path() / "hyprworks-invocation-XXXXXX").string();
    const int script = mkstemp(path.data());
    assert(script >= 0);
    const auto config = [&](const char* text) {
        assert(ftruncate(script, 0) == 0 && lseek(script, 0, SEEK_SET) == 0);
        const std::string_view content(text);
        assert(write(script, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
    };
    int pipe[2];
    assert(pipe2(pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    SignalSource signals;
    signals.fd = pipe[0];
    const auto request = [&](unsigned pending, bool fill = false) {
        signals.pending.fetch_or(pending, std::memory_order_relaxed);
        const char wake = 0;
        if (fill) {
            char bytes[4096]{};
            while (write(pipe[1], bytes, sizeof(bytes)) > 0) {}
            assert(errno == EAGAIN);
        } else assert(write(pipe[1], &wake, 1) == 1);
    };

    config("while true do end");
    {
        Invocation invocation(EventLoop::headless, &signals);
        request(SignalSource::reload);
        assert(invocation.load(path));
        assert(invocation.run() && invocation.reload_requested());
    }
    config("local hw = require('hyprworks')");
    {
        Invocation invocation(EventLoop::headless, &signals);
        assert(invocation.load(path));
        request(SignalSource::reload, true);
        signals.pending.fetch_or(SignalSource::quit, std::memory_order_relaxed);
        assert(invocation.run() && !invocation.reload_requested());
        assert(signals.pending.load() == 0);
    }
    char bytes[4096];
    while (read(pipe[0], bytes, sizeof(bytes)) > 0) {}
    config("require('hyprworks').quit()");
    {
        Invocation invocation(EventLoop::headless, &signals);
        request(SignalSource::reload);
        assert(invocation.load(path));
        assert(invocation.run() && !invocation.reload_requested());
    }
    config("local hw = require('hyprworks')");
    for (int i = 0; i < 32; ++i) {
        Invocation invocation;
        assert(invocation.load(path));
        std::thread requester([&] { invocation.request_reload(); });
        assert(invocation.run() && invocation.reload_requested());
        requester.join();
    }
    config("require('hyprworks').timer(1, function() error('callback failure') end)");
    {
        Invocation invocation;
        assert(invocation.load(path));
        const auto result = invocation.run();
        assert(!result && result.error().contains("callback failure") && !invocation.running());
    }
    close(pipe[0]); close(pipe[1]); close(script); unlink(path.c_str());
    std::cout << "Invocation core: busy interruption, saturated wake pipe, termination precedence, cross-thread requests and callback failure passed\n";
}
