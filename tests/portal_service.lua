local hw = require("hyprworks")
assert(hw.portal {max_fps = 30})
local window = assert(hw.window {width = 320, height = 240,
    app_id = "org.hyprworks.PortalFixture",
    title = "Hyprworks portal lifetime fixture " .. assert(os.getenv("DBUS_SESSION_BUS_ADDRESS"))})
assert(window:set_content {kind = "box", background = "#20a060"})
local bus = assert(hw.dbus {bus = "session"})
assert(bus:export {path = "/fixture", interface = "org.hyprworks.PortalFixture", timeout = 1000,
    methods = {{name = "RemoveSource"}, {name = "Quit"}}})
bus:on_event(function(_, event)
    if event.kind ~= "method" then return end
    assert(bus:reply(event.id, "", {}))
    if event.member == "RemoveSource" then window:close()
    else hw.quit(0) end
end)
assert(bus:request_name("org.hyprworks.PortalFixture"))
hw.timer(25000, function() error("Portal fixture timed out") end)
