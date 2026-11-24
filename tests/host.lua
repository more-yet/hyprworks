local hw = require("hyprworks")
local root = assert(hw.temp_dir(os.getenv("TMPDIR") or "/tmp"))
local path = assert(hw.temp_file(root))
local copy = root .. "/copy"
assert(hw.write_file(path, "old"))
assert(hw.update_file(path, 128, function(current)
    assert(not hw.lock_file(path .. ".lock"))
    assert(current == "old")
    return "new\0\255"
end))
assert(not pcall(hw.update_file, path, 128, function() error("aborted") end))
do
    local held <close> = assert(hw.lock_file(path .. ".lock"))
    assert(not hw.update_file(path, 128, function() error("entered locked editor") end))
    assert(hw.write_file(path, "new\0\255"))
    assert(not hw.lock_file(path .. ".lock"))
end
assert(not hw.copy_file(path, copy, 2))
assert(hw.copy_file(path, copy, 128))
assert(not hw.create_file(copy, "overwrite"))
local bytes = assert(hw.read_file(copy, 128))
assert(bytes == "new\0\255")
local results = 0
local function done()
    results = results + 1
    if results == 3 then
        for _, p in ipairs({copy, path, path .. ".lock", root}) do assert(os.remove(p)) end
        hw.quit(0)
    end
end
local child = assert(hw.process({"sh", "-ec", 'printf "%s" "$READY" >&2; exec cat'}, {
    timeout = 3000, output_limit = 128, input_limit = 3, env = {READY = "ready", PATH = "/usr/bin"},
}))
local received = ""
child:on_output(function(p, stream, chunk)
    if stream == "stdout" then received = received .. chunk; return end
    assert(chunk == "ready" and p:running())
    assert(p:write(bytes) == 3)
    assert(p:write("busy") == 0)
    p:on_drain(function(p, discarded)
        assert(discarded == 0)
        p:on_drain(nil)
        assert(p:write(bytes:sub(4)) == 2)
        assert(p:close_input())
        assert(not p:write("late"))
    end)
end)
child:on_exit(function(p, result)
    assert(result.outcome == "exited" and result.exit_code == 0 and result.stdin_discarded == 0)
    assert(received == bytes and result.stdout == bytes)
    done()
end)
local tty = assert(hw.process({"sh", "-ec", 'printf ready >/dev/tty; exec cat'}, {
    pty = {raw = true}, timeout = 3000, output_limit = 128,
}))
tty:on_output(function(p)
    assert(not p:close_input())
    assert(p:signal(15))
end)
tty:on_exit(function(_, result)
    assert(result.outcome == "signalled" and result.signal == 15)
    done()
end)
local hangup = assert(hw.process({"sh", "-ec", [[trap 'exit 23' HUP; printf ready; read answer || :]]}, {
    pty = {}, timeout = 3000, output_limit = 128,
}))
hangup:on_output(function(p) assert(p:hangup()) end)
hangup:on_exit(function(_, result)
    assert(result.outcome == "exited" and result.exit_code == 23)
    done()
end)
