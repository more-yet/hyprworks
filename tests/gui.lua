local hw = require("hyprworks")

local configured, loaded, cycles = false, false, 0
local window = assert(hw.window {
    width = 320, height = 160, title = "Hyprworks lifecycle check",
    on_configure = function(surface)
        configured = true
        local state = surface:state()
        assert(state.configured and state.width > 0 and state.height > 0)
    end,
})

local function content(label)
    return {
        kind = "column", gap = 4, padding = 8, background = "#20242aff",
        children = {
            {kind = "text", id = "label", text = label},
            {kind = "input", id = "entry", text = "Unicode: é — العربية"},
            {kind = "row", children = {
                {kind = "button", text = "Action"},
                {kind = "toggle", text = "Toggle"},
                {kind = "slider", id = "slider", value = 0.5, grow = 1},
            }},
        },
    }
end

assert(window:set_content(content("original")))
local old = assert(window:find("label"))
assert(old:set {text = "updated"})
assert(old:state().text == "updated")
assert(not pcall(function()
    window:set_content {kind = "column", children = {{kind = "not-a-kind"}}}
end))
assert(old:state().text == "updated")
assert(not window:set_content {kind = "row", children = {
    {kind = "text", id = "duplicate"}, {kind = "text", id = "duplicate"},
}})
assert(old:state().text == "updated")
assert(window:set_content(content("replacement")))
assert(not old:set {text = "stale"})
assert(window:find("entry"):focus())

local obsolete = assert(hw.window {width = 32, height = 32})
assert(obsolete:set_content {
    kind = "image",
    source = {data = string.rep("\255\0\0\128", 4096), width = 64, height = 64, channels = 4},
    on_loaded = function() error("closed image target received a completion") end,
})
obsolete:close()

local image = assert(hw.window {width = 32, height = 32})
assert(image:set_content {
    kind = "image", width = 32, height = 32,
    source = {data = "\255\0\0\128\0\255\0\255", width = 2, height = 1, channels = 4},
    on_loaded = function(element, event)
        assert(event.kind == "loaded" and not event.error, event.error)
        loaded = true
        assert(element:set {width = 24})
    end,
})

local popup, layer
hw.timer(50, function(timer)
    if not configured or not loaded then return end
    cycles = cycles + 1
    if cycles == 1 then
        popup = assert(hw.popup {parent = window, x = 10, y = 10, width = 80, height = 40})
        assert(popup:set_content {kind = "text", text = "Popup"})
        if hw.capabilities().layer_shell then
            layer = assert(hw.layer {height = 24})
            assert(layer:set_content {kind = "text", text = "Lifecycle check"})
        end
    elseif cycles == 2 then
        assert(window:close())
        assert(window:close())
        assert(popup:state().closed)
        assert(not window:set_content(content("closed")))
        if layer then layer:close() end
        image:close()
    elseif cycles < 45 then
        local transient = assert(hw.window {width = 16, height = 16})
        transient:close()
    else
        timer:close()
        print("GUI lifecycle: atomic replacement, stale handles, image cancellation, popup ownership and slot reuse passed")
        hw.quit()
    end
end, true)

hw.timer(8000, function() error("GUI lifecycle check timed out") end)
