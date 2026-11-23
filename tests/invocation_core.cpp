#include "../src/runtime/invocation.hpp"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace hyprworks::lua;

int main() {
    {
        InvocationState state;
        state.lua = luaL_newstate();
        assert(state.lua);
        *static_cast<InvocationState**>(lua_getextraspace(state.lua)) = &state;
        luaL_openlibs(state.lua);
        lua_newtable(state.lua); register_host(state.lua); register_workspaces(state.lua);
        lua_setglobal(state.lua, "hw");
        const auto script = [&](const char* source) {
            if (luaL_dostring(state.lua, source) != LUA_OK) {
                std::cerr << lua_tostring(state.lua, -1) << '\n'; std::abort();
            }
        };
        lua_pushcfunction(state.lua, guarded<+[](lua_State* lua) {
            identifier(lua, check_uint64(lua, 1, 1));
            return 1;
        }>);
        lua_setglobal(state.lua, "identifier");
        script(R"(
            assert(identifier("18446744073709551615") == "18446744073709551615")
            for _, value in ipairs({"0", "-1", "18446744073709551616", "1x", "1\0", 1}) do
                assert(not pcall(identifier, value))
            end
        )");
        script(R"(
            collectgarbage("stop")
            delivered, finalized = 0, 0
            callback_timer = hw.timer(0, function() error("obsolete callback delivered") end)
            local garbage = setmetatable({}, {__gc = function()
                finalized = finalized + 1
                callback_timer:on_tick(function(timer, ticks)
                    assert(timer == callback_timer and ticks == 1)
                    delivered = delivered + 1
                end)
            end})
        )");
        auto& timer = *state.timers.front();
        assert(dispatch_callback(state.lua, timer, timer.callback, timer.generation, [](lua_State* lua) {
            lua_gc(lua, LUA_GCCOLLECT);
            lua_pushinteger(lua, 1);
            return 1;
        }) == 0);
        assert(lua_gettop(state.lua) == 0);
        script("assert(finalized == 1 and delivered == 0)");
        timer.pending = 1;
        assert(dispatch_utilities(state.lua, state) == 0);
        script(R"(
            assert(delivered == 1)
            local garbage = setmetatable({}, {__gc = function()
                finalized = finalized + 1
                callback_timer:close()
            end})
        )");
        assert(dispatch_callback(state.lua, timer, timer.callback, timer.generation, [](lua_State* lua) {
            lua_gc(lua, LUA_GCCOLLECT);
            lua_pushinteger(lua, 1);
            return 1;
        }) == 0);
        assert(timer.closed && lua_gettop(state.lua) == 0);
        script("assert(finalized == 2 and delivered == 1); collectgarbage('restart')");
        script(R"(
            local replacement = hw.timer(0, function() error("retired slot delivered") end)
            callback_timer:start(0)
            callback_timer:on_tick(function() delivered = delivered + 1 end)
            replacement:close()
        )");
        timer.pending = 1;
        assert(dispatch_utilities(state.lua, state) == 0);
        script("assert(delivered == 2); callback_timer:close()");

        auto retained = std::make_shared<WorkspaceBinding>();
        auto& workspace = *retained;
        assert(open_active(state.lua, vacant_slot(state.workspaces), retained, "hyprworks.workspaces") == 1);
        lua_setglobal(state.lua, "workspace");
        script(R"(
            workspace:on_status(function(connection)
                assert(connection:close().id == "17")
            end)
            workspace:on_change(function() error("closed snapshot delivered") end)
            workspace:on_result(function() error("closed result delivered") end)
        )");
        workspace.result = native::SwitchResult{17, native::SwitchOutcome::cancelled, false, {}};
        assert(dispatch_workspaces(state.lua, state) == 0);
        assert(workspace.closed && !workspace.result && lua_gettop(state.lua) == 0);
        state.closing = true;
        lua_close(state.lua); state.lua = nullptr;
    }
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
    config("require('hyprworks').quit(0)");
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
        local options = setmetatable({timeout = 1000, output_limit = 1024}, {
            __index = function() error("option lookup invoked author code") end,
        })
        local first = assert(hw.process({"/bin/sh", "-c", "exec 1>&- 2>&-; sleep 0.03"}, options))
        local second = assert(hw.process({"/bin/sh", "-c", "sleep 0.01; printf ready"}, options))
        local function exited(process)
            local result = process:result()
            assert(result.outcome == "exited" and result.exit_code == 0)
            if process == second then assert(result.stdout == "ready") end
            finished = finished + 1
            if finished == 2 then assert(ticks > 0); hw.quit(0) end
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
                hw.quit(0)
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
        assert(process.start(arguments, {.directory = {}, .timeout = std::chrono::seconds(3), .output_limit = 1024}));
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
    std::cout << "Invocation core: finalizer reentrancy, result ownership, interruption, saturated wake pipe, termination precedence, cross-thread requests, callback failure and stale readiness passed\n";
}
