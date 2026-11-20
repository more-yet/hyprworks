local hw = require("hyprworks")
local server = assert(hw.dbus {bus = "session"})
local client = assert(hw.dbus {bus = "session"})
local destination, path, interface = "org.hyprworks.Test", "/org/hyprworks/Test", "org.hyprworks.Test"
local pending, methods, expired, finished = {}, 0, false, false
assert(server:export {path = path, interface = interface, timeout = 40, methods = {
    {name = "Echo", input = "a{sv}t", output = "a{sv}t"}, {name = "Wait"},
}})
local function request(member, signature, arguments, done)
    local id = assert(client:call {destination = destination, path = path, interface = interface,
        member = member, signature = signature or "", arguments = arguments or {}, timeout = 1000})
    pending[id] = done
    return id
end
server:on_event(function(_, event)
    if event.kind == "method" then
        methods = methods + 1
        if event.member == "Echo" then
            assert(not server:reply(event.id, "", {}))
            assert(server:reply(event.id, "a{sv}t", event.arguments))
            assert(not server:reply(event.id, "a{sv}t", event.arguments))
        end
    elseif event.kind == "expired" then
        expired = true
        assert(not server:reply(event.id, "", {}))
    elseif event.kind == "reply" then
        assert(not event.error, event.error)
        local cancelled = request("Echo", "a{sv}t", {{payload = {signature = "ay", value = "\0\255"}}, "18446744073709551615"},
            function() error("canceled call was delivered") end)
        assert(client:cancel(cancelled))
        pending[cancelled] = nil
        request("Echo", "a{sv}t", {{payload = {signature = "ay", value = "\0\255"}}, "18446744073709551615"}, function(reply)
            assert(not reply.error, reply.error)
            assert(reply.arguments[1].payload.value == "\0\255")
            assert(reply.arguments[2] == "18446744073709551615")
            request("Wait", "", {}, function(timeout)
                assert(timeout.error_name == "org.freedesktop.DBus.Error.NoReply")
                finished = true
            end)
        end)
    end
end)
client:on_event(function(_, event)
    if event.kind == "reply" then
        assert(pending[event.id], "unexpected reply")(event)
        pending[event.id] = nil
    end
end)
assert(server:call {destination = "org.freedesktop.DBus", path = "/org/freedesktop/DBus",
    interface = "org.freedesktop.DBus", member = "RequestName", signature = "su", arguments = {destination, 0}})
hw.timer(1000, function()
    assert(finished and expired and methods == 3)
    client:close()
    server:close()
    assert(client:status().phase == "closed")
    print("D-Bus asynchronous reply, cancellation and expiration checks passed")
    hw.quit()
end)
