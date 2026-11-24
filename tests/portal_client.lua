local hw = require("hyprworks")
local backend, path = "org.freedesktop.impl.portal.desktop.hyprworks", "/org/freedesktop/portal/desktop"
local screen, control = "org.freedesktop.impl.portal.ScreenCast", "org.hyprworks.Portal"
local frontend = assert(hw.dbus {bus = "session"})
local ui
local pending, phase, revision, source, token = {}, "waiting", nil, nil, nil
local ui_ready, frontend_ready, refreshing, refresh_again = false, false, false, false
local session_path = path .. "/session/test/stream"
local request_path = path .. "/request/test/start"
local refresh, connect_ui, begin, cancel_request

local function call(bus, interface, member, signature, arguments, done, target, destination)
    local id = assert(bus:call {destination = destination or backend, path = target or path, interface = interface,
        member = member, signature = signature, arguments = arguments, timeout = 15000})
    pending[bus] = pending[bus] or {}
    pending[bus][id] = done
end
local function reply(bus, event)
    local done = assert(pending[bus][event.id])
    pending[bus][event.id] = nil
    done(event)
end
local function success(event) assert(not event.error, event.error); return event.arguments end
local function choose()
    call(ui, control, "Choose", "ttas", {token, "0", {source}}, function(stale)
        assert(stale.error)
        call(ui, control, "Choose", "ttas", {token, revision, {source}}, success)
    end)
end

refresh = function()
    if not ui_ready then return end
    if refreshing then refresh_again = true; return end
    refreshing = true
    call(ui, control, "Snapshot", "", {}, function(event)
        refreshing = false
        local data = success(event)
        revision = data[1]
        if phase == "abandoning" and #data[2] == 0 then
            print("Portal: UI reconnect, stale selection, PipeWire frames, source removal, cancellation and caller disappearance passed")
            hw.quit(0)
            return
        end
        for _, item in ipairs(data[3]) do
            if item.title.value:find("Hyprworks portal lifetime fixture " .. assert(os.getenv("DBUS_SESSION_BUS_ADDRESS")), 1, true) then
                source = item.id.value
            end
        end
        if phase == "waiting" and source and frontend_ready then begin(); return end
        for _, request in ipairs(data[2]) do
            if request.phase.value == "choosing" then
                if phase == "starting" then
                    token, phase = request.id.value, "reconnecting"
                    ui_ready = false
                    ui:close()
                    connect_ui()
                elseif phase == "reconnecting" then
                    assert(request.id.value == token)
                    phase = "streaming"
                    choose()
                elseif phase == "cancelling" then
                    phase = "closing_request"
                    token = request.id.value
                    call(frontend, "org.freedesktop.impl.portal.Request", "Close", "", {}, success,
                        path .. "/request/test/cancel")
                end
            end
        end
        if refresh_again then refresh_again = false; refresh() end
    end)
end

cancel_request = function()
    local session = path .. "/session/test/cancel"
    call(frontend, screen, "CreateSession", "oosa{sv}", {path .. "/request/test/create_cancel", session, "org.hyprworks.Test", {}}, function(created)
        assert(success(created)[1] == 0)
        call(frontend, screen, "SelectSources", "oosa{sv}", {path .. "/request/test/select_cancel", session, "org.hyprworks.Test",
            {types = {signature = "u", value = 1}}}, function(selected)
            assert(success(selected)[1] == 0)
            phase = "cancelling"
            call(frontend, screen, "Start", "oossa{sv}", {path .. "/request/test/cancel", session, "org.hyprworks.Test", "", {}}, function(cancelled)
                assert(success(cancelled)[1] == 1)
                call(ui, control, "Choose", "ttas", {token, revision, {source}}, function(stale)
                    assert(stale.error)
                    call(frontend, screen, "CreateSession", "oosa{sv}",
                        {path .. "/request/test/abandon", path .. "/session/test/abandon", "org.hyprworks.Test", {}}, function(created)
                        assert(success(created)[1] == 0)
                        phase = "abandoning"
                        frontend:close()
                        refresh()
                    end)
                end)
            end)
        end)
    end)
end

connect_ui = function()
    ui = assert(hw.dbus {bus = "session"})
    assert(ui:subscribe("type='signal',sender='" .. backend .. "',interface='" .. control .. "'"))
    ui:on_event(function(bus, event)
        if event.kind == "name_acquired" then
            call(bus, control, "Attach", "", {}, function(attached)
                success(attached); ui_ready = true; refresh()
            end)
        elseif event.kind == "reply" then reply(bus, event)
        elseif event.kind == "signal" then refresh()
        elseif event.kind == "name_failed" then error(event.error) end
    end)
    assert(ui:request_name("org.hyprworks.PortalUI"))
end

begin = function()
    phase = "creating"
    call(frontend, screen, "CreateSession", "oosa{sv}", {path .. "/request/test/create", session_path, "org.hyprworks.Test", {}}, function(created)
        assert(success(created)[1] == 0)
        call(frontend, screen, "SelectSources", "oosa{sv}", {path .. "/request/test/select", session_path, "org.hyprworks.Test",
            {types = {signature = "u", value = 2}, cursor_mode = {signature = "u", value = 2}}}, function(selected)
            assert(success(selected)[1] == 0)
            phase = "starting"
            call(frontend, screen, "Start", "oossa{sv}", {request_path, session_path, "org.hyprworks.Test", "", {}}, function(started)
                local data = success(started)
                assert(data[1] == 0, "portal could not start capture")
                local node = data[2].streams.value[1][1]
                local consumer = assert(hw.process({assert(os.getenv("HYPRWORKS_PIPEWIRE_CHECK")), tostring(node)},
                    {timeout = 10000, output_limit = 65536}))
                consumer:on_exit(function(_, result)
                    assert(result.outcome == "exited" and result.exit_code == 0,
                        result.outcome .. " signal=" .. tostring(result.signal) .. " " .. result.stderr)
                    phase = "removing"
                    call(frontend, "org.hyprworks.PortalFixture", "RemoveSource", "", {}, success,
                        "/fixture", "org.hyprworks.PortalFixture")
                end)
            end)
        end)
    end)
end

assert(frontend:subscribe("type='signal',sender='" .. backend .. "',interface='org.freedesktop.impl.portal.Session'"))
frontend:on_event(function(bus, event)
    if event.kind == "name_acquired" then frontend_ready = true; refresh()
    elseif event.kind == "reply" then reply(bus, event)
    elseif event.kind == "signal" and event.member == "Closed" and event.path == session_path then
        assert(phase == "removing")
        call(ui, control, "Snapshot", "", {}, function(snapshot)
            assert(#success(snapshot)[2] == 0)
            phase = "cancel_setup"
            cancel_request()
        end)
    elseif event.kind == "name_failed" then error(event.error) end
end)
assert(frontend:request_name("org.freedesktop.portal.Desktop"))
connect_ui()
hw.timer(20000, function() error("Portal lifecycle timed out in " .. phase) end)
