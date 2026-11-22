local hw = require("hyprworks")
local agent = assert(hw.polkit())
local active

local function dismiss(view)
    view.prompt = nil
    assert(view.surface:find("response"):set {text = ""})
    view.surface:close()
    active = nil
end

local function cancel(view)
    assert(agent:cancel(view.id))
    dismiss(view)
end

local function submit(view)
    if not view.prompt then return end
    local field = view.surface:find("response")
    local response = field:state().text
    assert(field:set {text = ""})
    local ok, err = agent:respond(view.id, view.prompt, response)
    response = nil
    if ok then
        view.prompt = nil
        assert(field:set {enabled = false})
        assert(view.surface:find("submit"):set {enabled = false})
    end
    assert(view.surface:find("notice"):set {text = ""})
    assert(view.surface:find("feedback"):set {text = ok and "" or err})
end

local function authenticate(view, identity)
    assert(agent:authenticate(view.id, identity.uid))
    local name = identity.name ~= "" and identity.name or tostring(identity.uid)
    assert(view.surface:find("identities"):replace_children {
        {kind = "text", text = "Authenticate as " .. name, wrap = true},
    })
    assert(view.surface:find("prompt"):set {text = "Waiting for authentication…"})
end

agent:on_status(function(handle)
    local status = handle:status()
    if status.phase == "failed" then
        if active then dismiss(active) end
        error(status.error)
    end
end)

agent:on_event(function(_, event)
    if event.kind == "request" then
        local view = {id = event.id}
        view.surface = assert(hw.layer {
            app_id = "hyprworks-polkit", edge = "all", layer = "overlay",
            width = 480, height = 400, keyboard = "exclusive", dismiss_on_escape = true,
            on_close = function() cancel(view) end,
            on_input = function(_, input)
                if input.kind == "key" and (input.key == "Return" or input.key == "KP_Enter") and
                    view.prompt and view.surface:find("response"):accessibility().focused then
                    if input.pressed and not input.repeated then submit(view) end
                    return true
                end
            end,
        })
        local identities = {}
        for index, identity in ipairs(event.identities) do
            identities[index] = {
                kind = "button", id = "identity-" .. index,
                text = identity.name ~= "" and identity.name or tostring(identity.uid),
                height = 36, padding = 8, radius = 6, background = "#343c48",
                on_activate = function() authenticate(view, identity) end,
            }
        end
        assert(view.surface:set_content {
            kind = "column", padding = 24, gap = 16, background = "#202630",
            radius = 12, border_width = 1, border_color = "#485264",
            children = {
                {kind = "text", text = "Authentication required", font = "Sans Bold 16"},
                {kind = "scroll", grow = 1, gap = 12, children = {
                    {kind = "text", text = event.message, wrap = true},
                    {kind = "text", text = event.action, wrap = true, font = "Sans 10", color = "#b8c0cc"},
                    {kind = "column", id = "identities", gap = 8, children = identities},
                    {kind = "text", id = "notice", wrap = true, color = "#b8c0cc"},
                    {kind = "text", id = "feedback", wrap = true, color = "#ff9090"},
                    {kind = "text", id = "prompt", text = "Choose an account", wrap = true},
                    {kind = "input", id = "response", enabled = false, password = true,
                        height = 40, padding = 10, radius = 6, background = "#141a22",
                        border_width = 1, border_color = "#485264",
                        accessibility = {name = "Authentication response"}},
                }},
                {kind = "row", gap = 12, children = {
                    {kind = "button", text = "Cancel", grow = 1, height = 40,
                        radius = 6, background = "#343c48", on_activate = function() cancel(view) end},
                    {kind = "button", id = "submit", text = "Authenticate", enabled = false,
                        grow = 1, height = 40, radius = 6, background = "#8ab4f8", color = "#141a22",
                        on_activate = function() submit(view) end},
                }},
            },
        })
        active = view
        if #event.identities == 1 then authenticate(view, event.identities[1])
        else assert(view.surface:find("identity-1"):focus()) end
        return
    end

    local view = active
    if not view or view.id ~= event.id then return end
    if event.kind == "prompt" then
        view.prompt = event.prompt
        assert(view.surface:find("prompt"):set {text = event.text})
        local field = view.surface:find("response")
        assert(field:set {text = "", password = not event.echo, enabled = true,
            accessibility = {name = event.text}})
        assert(view.surface:find("submit"):set {enabled = true})
        assert(field:focus())
    elseif event.kind == "info" or event.kind == "error" then
        assert(view.surface:find(event.kind == "error" and "feedback" or "notice"):set {text = event.text})
    elseif event.kind == "completed" then
        dismiss(view)
    end
end)
