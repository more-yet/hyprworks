local hw = require("hyprworks")
assert(hw.capabilities().workspace_capture, "native workspace capture is required")
local workspaces = assert(hw.hyprland.workspaces())
local started, remaining = false, 0
local sources, target = {}, nil

workspaces:on_change(function()
    if started then return end
    local snapshot = workspaces:snapshot()
    if not snapshot then return end
    local outputs = {}
    for _, output in ipairs(snapshot.outputs) do outputs[output.id] = output.name end
    local targets = {}
    for _, workspace in ipairs(snapshot.workspaces) do
        if workspace.type == "normal" and outputs[workspace.output_id] and #targets < 4 then
            targets[#targets + 1] = {name = workspace.name, output = outputs[workspace.output_id]}
        end
    end
    assert(#targets > 0, "a normal workspace is required")
    target = targets[1]
    started = true
    assert(not hw.hyprland.capture_workspace({name = targets[1].name, output = "missing-output"}, function()
        error("missing output delivered a capture")
    end))
    remaining = #targets * 2
    local function next_capture()
        local index = #targets * 2 - remaining
        local target = targets[index // 2 + 1]
        assert(hw.hyprland.capture_workspace({
            name = target.name, output = target.output,
            mode = index % 2 == 0 and "windows_only" or "everything",
            max_width = 160, max_height = 90, max_fps = 2,
        }, function(capture, event)
            assert(event.kind == "frame", event.error)
            assert(event.width > 0 and event.width <= 160 and event.height > 0 and event.height <= 90)
            assert(event.source_width >= event.width and event.source_height >= event.height)
            sources[#sources + 1] = event.source
            capture:close()
            remaining = remaining - 1
            if remaining > 0 then next_capture() end
        end))
    end
    next_capture()
    local windows = assert(hw.hyprland.windows())
    windows:close()
end)

hw.timer(25, function(timer)
    if not started or remaining ~= 0 then timer:start(25); return end
    local cancelled = {}
    for index = 1, 8 do
        cancelled[index] = assert(hw.hyprland.capture_workspace(target, function() error("cancelled capture delivered") end))
    end
    assert(not hw.hyprland.capture_workspace(target, function() error("over-limit capture delivered") end))
    for _, capture in ipairs(cancelled) do capture:close() end
    local surface = assert(hw.layer {width = 160, height = 90, keyboard = "none"})
    assert(surface:set_content {kind = "image", source = sources[1]})
    hw.timer(100, function()
        surface:close()
        workspaces:close()
        print("Workspace captures: compositor frames, modes, catalog independence, retained images, limits and cancellation passed")
        hw.quit()
    end)
end)
hw.timer(8000, function() error("workspace capture check timed out") end)
