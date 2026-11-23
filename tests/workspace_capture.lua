local hw = require("hyprworks")
assert(hw.capabilities().workspace_capture, "native workspace capture is required")
local workspaces = assert(hw.hyprland.workspaces())
workspaces:on_change(function()
    local snapshot = workspaces:snapshot()
    if not snapshot then return end
    local outputs = {}
    for _, output in ipairs(snapshot.outputs) do outputs[output.id] = output.name end
    local target
    for _, workspace in ipairs(snapshot.workspaces) do
        if workspace.type == "normal" and outputs[workspace.output_id] then
            target = {name = workspace.name, output = outputs[workspace.output_id], mode = "windows_only",
                max_width = 160, max_height = 90, max_fps = 2}
            break
        end
    end
    assert(target, "a normal workspace on a known output is required")
    workspaces:on_change(nil)
    assert(hw.hyprland.capture_workspace(target, function(capture, event)
        assert(event.kind == "frame", event.error)
        capture:close()
        local surface = assert(hw.layer {width = 160, height = 90, keyboard = "none", edge = "top", layer = "top"})
        assert(surface:set_content {kind = "image", source = event.source})

        local slots = {}
        local function retired() error("retired capture delivered a frame") end
        for i = 1, 8 do slots[i] = assert(hw.hyprland.capture_workspace(target, retired)) end
        assert(not hw.hyprland.capture_workspace(target, retired))
        for _, handle in ipairs(slots) do handle:close() end
        hw.timer(100, function()
            surface:close(); workspaces:close()
            print("Workspace capture: catalog independence, retained frame, capacity and cancellation passed")
            hw.quit(0)
        end)
    end))
    assert(hw.hyprland.windows()):close()
end)
hw.timer(8000, function() error("workspace capture check timed out") end)
