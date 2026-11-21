#include <hyprworks/lua.hpp>
#include <hyprworks/host.hpp>
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
    config(R"(
        local hw = require('hyprworks')
        local finished, ticks = 0, 0
        local first = assert(hw.process {"/bin/sh", "-c", "exec 1>&- 2>&-; sleep 0.03"})
        local second = assert(hw.process {"/bin/sh", "-c", "sleep 0.01; printf ready"})
        local function exited(process)
            local result = process:result()
            assert(result.outcome == "exited" and result.exit_code == 0)
            if process == second then assert(result.stdout == "ready") end
            finished = finished + 1
            if finished == 2 then assert(ticks > 0); hw.quit() end
        end
        first:on_exit(exited)
        second:on_exit(exited)
        hw.timer(1, function() ticks = ticks + 1 end, true)
        hw.timer(1000, function() error('sparse readiness timeout') end)
    )");
    {
        Invocation invocation;
        assert(invocation.load(path));
        assert(invocation.run());
    }
    config(R"(
        local hw = require('hyprworks')
        assert(hw.on_clock_change(function() error('obsolete clock callback') end))
        assert(hw.on_clock_change(nil))
        assert(hw.on_clock_change(function()
            assert(hw.on_clock_change(function()
                assert(hw.on_clock_change(nil))
                hw.quit()
            end))
        end))
        hw.timer(1000, function() error('clock callback timeout') end)
    )");
    {
        Invocation invocation;
        assert(invocation.load(path));
        assert(invocation.run());
    }
    {
        hyprworks::host::Process process;
        const std::string arguments[]{"/bin/sh", "-c", "printf ready"};
        assert(process.start(arguments));
        const auto deadline = hyprworks::host::Clock::now() + std::chrono::seconds(3);
        while (process.running()) {
            assert(hyprworks::host::Clock::now() < deadline);
            auto stale = process.prepare_wait();
            auto current = process.prepare_wait();
            assert(poll(current.fds.data(), current.fds.size(), 100) >= 0);
            for (std::size_t i = 0; i < current.fds.size(); ++i) {
                assert(stale.fds[i].fd == current.fds[i].fd);
                stale.fds[i].revents = current.fds[i].revents;
            }
            assert(!process.dispatch(stale) && !process.result());
            process.dispatch(current);
        }
        assert(process.result()->outcome == hyprworks::host::ProcessOutcome::exited);
        assert(process.result()->stdout_text == "ready");
    }
    close(pipe[0]); close(pipe[1]); close(script); unlink(path.c_str());
    std::cout << "Invocation core: interruption, saturated wake pipe, termination precedence, cross-thread requests, callback failure and stale readiness passed\n";
}
