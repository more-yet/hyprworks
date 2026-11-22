local hw = require("hyprworks")
local agent = assert(hw.polkit())
local requested, prompted, completed = false, false, false
local pid = assert(hw.read_file("/proc/self/stat")):match("^(%d+)")

agent:on_status(function(handle)
    local status = handle:status()
    assert(status.phase ~= "failed", status.error)
    if status.phase ~= "ready" then return end
    local check = assert(hw.process({
        "pkcheck", "--action-id", "org.freedesktop.policykit.exec",
        "--process", pid, "--allow-user-interaction",
    }, {timeout = 10000}))
    check:on_exit(function(_, result)
        assert(result.outcome == "exited" and result.exit_code ~= 0, result.stderr)
        assert(requested and prompted and completed)
        agent:close()
        print("Polkit live registration, PAM prompt and cancellation passed")
        hw.quit()
    end)
end)
agent:on_event(function(handle, event)
    assert(event.cookie == nil)
    if event.kind == "request" then
        assert(not requested and event.action == "org.freedesktop.policykit.exec")
        requested = true
        assert(handle:authenticate(event.id, event.identities[1].uid))
    elseif event.kind == "prompt" then
        assert(not event.echo and #event.text > 0)
        prompted = true
        assert(handle:cancel(event.id))
        assert(not handle:respond(event.id, event.prompt, ""))
    elseif event.kind == "completed" then
        assert(event.cancelled and not event.authorized)
        completed = true
    end
end)
hw.timer(12000, function() error("Polkit live check timed out") end)
