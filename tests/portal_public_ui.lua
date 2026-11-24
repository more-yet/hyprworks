local hw = require("hyprworks")
local bus = assert(hw.dbus {bus = "session"})
local backend, interface = "org.freedesktop.impl.portal.desktop.hyprworks", "org.hyprworks.Portal"
local pending, chosen = {}, {}
local function call(member, signature, arguments, done)
    local id = assert(bus:call {destination = backend, path = "/org/freedesktop/portal/desktop", interface = interface,
        member = member, signature = signature, arguments = arguments, timeout = 3000})
    pending[id] = done
end
local function snapshot(event)
    assert(not event.error, event.error)
    local revision, requests, sources = table.unpack(event.arguments)
    for _, request in ipairs(requests) do
        if request.phase.value == "choosing" and not chosen[request.id.value] then
            local source
            for _, item in ipairs(sources) do
                if item.type.value & request.types.value ~= 0 and
                    (item.type.value == 1 or item.title.value:find(
                        "Hyprworks portal lifetime fixture " .. assert(os.getenv("DBUS_SESSION_BUS_ADDRESS")), 1, true)) then
                    source = item.id.value
                    break
                end
            end
            if not source then return end
            chosen[request.id.value] = true
            call("Choose", "ttas", {request.id.value, revision, {source}}, function(reply)
                assert(not reply.error, reply.error)
            end)
        end
    end
end
assert(bus:subscribe("type='signal',sender='" .. backend .. "',interface='" .. interface .. "'"))
bus:on_event(function(_, event)
    if event.kind == "name_acquired" then call("Attach", "", {}, snapshot)
    elseif event.kind == "signal" then call("Snapshot", "", {}, snapshot)
    elseif event.kind == "reply" then
        local done = assert(pending[event.id]); pending[event.id] = nil; done(event)
    elseif event.kind == "name_failed" then error(event.error) end
end)
assert(bus:request_name("org.hyprworks.PortalUI"))
