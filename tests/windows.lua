local hw = require("hyprworks")
local windows = assert(hw.hyprland.windows())
local window = assert(hw.window {
    width = 640, height = 360, app_id = "org.hyprworks.WindowCheck", title = "Window subscription check",
})
assert(window:set_content {kind = "text", text = "Capture lifecycle", background = "#223344"})

local stage, frames, original_id, unavailable = 0, 0, nil, false
windows:on_status(function()
    local status = windows:status()
    assert(status.phase ~= "failed", status.error)
end)
windows:on_change(function()
    local snapshot = windows:snapshot()
    if not snapshot then return end
    local target
    for _, record in ipairs(snapshot.windows) do
        if record.class == "org.hyprworks.WindowCheck" then target = record; break end
    end
    if not target then return end
    assert(target.mapped and target.width > 0 and target.height > 0)
    if stage == 0 then
        stage, original_id = 1, target.id
        assert(windows:capture(target.id, {max_width = 160, max_height = 90, max_fps = 2}, function(capture, event)
            assert(stage == 1 and event.kind == "frame", event.error)
            assert(event.width <= 160 and event.height <= 90)
            frames = frames + 1
            capture:close()
            assert(window:set_content {kind = "image", source = event.source})
            stage = 2
            assert(windows:restart())
            assert(windows:snapshot() == nil)
        end))
    elseif stage == 2 then
        assert(target.id ~= original_id)
        assert(not windows:capture(original_id, {}, function() error("stale identity captured") end))
        local cancelled = {}
        for i = 1, 8 do
            cancelled[i] = assert(windows:capture(target.id, {}, function() error("cancelled capture callback") end))
        end
        assert(not windows:capture(target.id, {}, function() error("over-limit capture callback") end))
        for _, capture in ipairs(cancelled) do capture:close() end
        assert(windows:capture(target.id, {}, function(capture, event)
            assert(stage == 3 and event.kind == "unavailable", event.error)
            unavailable = true
            capture:close()
        end))
        stage = 3
        window:close()
    end
end)
hw.timer(25, function(timer)
    if stage ~= 3 or not unavailable then timer:start(25); return end
    local snapshot = windows:snapshot()
    for _, record in ipairs(snapshot and snapshot.windows or {}) do
        if record.class == "org.hyprworks.WindowCheck" then timer:start(25); return end
    end
    assert(frames == 1)
    windows:close()
    print("Windows: snapshot, capture lifetime/limits, restart identities, cancellation and unavailability passed")
    hw.quit()
end)
hw.timer(8000, function() error("window subscription check timed out") end)
