local hw = require("hyprworks")
local server = assert(hw.dbus {bus = "session"})
local client = assert(hw.dbus {bus = "session"})
local name, path = "org.hyprworks.ServiceTest", "/org/hyprworks/ServiceTest"
local pending, expired, released = {}, false, false
local function finish()
    if expired and released then
        client:close()
        server:close()
        hw.quit(0)
    end
end
local function call(member, signature, arguments, done)
    local id = assert(client:call {
        destination = name, path = path, interface = name, member = member,
        signature = signature, arguments = arguments, timeout = 1000,
    })
    pending[id] = done
    return id
end
assert(server:export {
    path = path, interface = name, timeout = 40,
    methods = {{name = "Fd", input = "h", output = "h"}, {name = "Hold"}, {name = "Wait"}},
})
client:on_event(function(_, event)
    if event.kind == "reply" then
        local done = assert(pending[event.id])
        pending[event.id] = nil
        done(event)
    end
end)
server:on_event(function(_, event)
    if event.kind == "name_acquired" then
        local canceled = call("Hold", "", {}, function() error("canceled reply delivered") end)
        assert(client:cancel(canceled))
        pending[canceled] = nil
        local fd = assert(hw.dbus_fd(1))
        call("Fd", "h", {fd}, function(response)
            assert(not response.error, response.error)
            local received = response.arguments[1]
            collectgarbage("collect")
            assert(received:fileno() >= 0)
            received:close()
            assert(received:fileno() == -1)
            received:close()
            call("Wait", "", {}, function(timeout)
                assert(timeout.error_name == "org.freedesktop.DBus.Error.NoReply")
                assert(server:release_name(name))
            end)
        end)
        fd:close()
        collectgarbage("collect")
    elseif event.kind == "method" then
        if event.member == "Fd" then
            assert(server:reply(event.id, "h", event.arguments))
            event.arguments[1]:close()
        elseif event.member == "Hold" then
            assert(server:reply(event.id, "", {}))
        elseif event.member == "Wait" then
            assert(os.execute("sleep 0.08"))
            assert(not server:reply(event.id, "", {}))
            assert(not server:error(event.id, "org.hyprworks.Error", "late"))
        end
    elseif event.kind == "expired" then
        assert(not server:reply(event.id, "", {}))
        expired = true
        finish()
    elseif event.kind == "name_lost" then
        released = true
        finish()
    end
end)
local retired = {}
for i = 1, 12 do
    retired[i] = assert(hw.dbus {bus = "session"})
    retired[i]:close()
end
for _, bus in ipairs(retired) do bus:close() end
retired = nil
collectgarbage("collect")
assert(server:request_name(name))
hw.timer(5000, function() error("D-Bus service lifecycle timed out") end)
