local hw = require("hyprworks")
local b = assert(hw.dbus {bus = "session"})
local name = "org.hyprworks.TestTerminal"
local child = assert(hw.process({"sh", "-ec", [[
    read -r a </dev/tty; read -r b </dev/tty; printf '%s:%s:%s' "$a" "$b" "$(stty size)"
]]}, {pty = {echo = false}, timeout = 5000, output_limit = 128}))
assert(b:export {path = "/terminal", interface = name, timeout = 3000,
    methods = {{name = "Answer", input = "s"}, {name = "Finish", input = "s", output = "s"}}})
assert(b:call {destination = "org.freedesktop.DBus", path = "/org/freedesktop/DBus",
    interface = "org.freedesktop.DBus", member = "RequestName", signature = "su", arguments = {name, 4}, timeout = 3000})
b:on_event(function(_, e)
    if e.kind == "reply" then assert(not e.error and e.arguments[1] == 1); return end
    if e.kind ~= "method" then return end
    if e.member == "Finish" then assert(child:resize(30, 90)) end
    local line = e.arguments[1] .. "\n"
    assert(child:write(line) == #line)
    if e.member == "Answer" then assert(b:reply(e.id, "", {})); return end
    child:on_exit(function(_, result)
        assert(result.outcome == "exited" and result.exit_code == 0)
        assert(b:reply(e.id, "s", {result.stdout}))
    end)
end)
