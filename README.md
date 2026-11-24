# Hyprworks

A C++ runtime for building Hyprland desktop shells in Lua.

Lua owns desktop composition, appearance and interaction policy. Hyprworks owns
native connections, element state, input, rendering and asynchronous work.
The runtime brings those resources together in one invocation and event loop.
Shell composition, dialogs, preferences and themes live in your configuration
directory. [ARCHITECTURE.md](ARCHITECTURE.md) describes native ownership,
publication, callback admission and cleanup.

## Build and run

Use a C++23 compiler, CMake, Ninja and pkg-config. The build resolves system
libraries for Wayland, Cairo/Pango, image decoding, Lua, json-c, sd-bus, GIO,
libpulse, libpolkit-agent and PipeWire. Dependency versions and feature options
live in [CMakeLists.txt](CMakeLists.txt).

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DHYPRWORKS_BUILD_TESTS=ON
cmake --build build/local
ctest --test-dir build/local --output-on-failure
HYPRWORKS_CONFIG_DIR=/path/to/shell build/local/hyprworks
```

`HYPRWORKS_BUILD_GUI=OFF` builds a headless runtime.
`HYPRWORKS_BUILD_LUA=OFF` builds the native library.
Desktop integrations have individual `HYPRWORKS_BUILD_<FEATURE>` options.

### Configuration directory

Set `HYPRWORKS_CONFIG_DIR` to the absolute path of your configuration directory.
`hyprworks` loads `main.lua` from that directory.

The package installs the `60-hyprworks-environment` systemd user-environment
generator. At user-manager startup or reload, after `environment.d` processing,
it initializes `HYPRWORKS_CONFIG_DIR` to `$XDG_CONFIG_HOME/hyprworks` when the
variable is unset and `XDG_CONFIG_HOME` is absolute.
User services and their child processes inherit the result;
independently started shells need to supply the variable in their own environment.

`--entry FILE` selects another filename in that directory, such as
`polkit-agent.lua`.

```sh
HYPRWORKS_CONFIG_DIR=/path/to/shell hyprworks
HYPRWORKS_CONFIG_DIR=/path/to/shell hyprworks --entry polkit-agent.lua
```

Modules resolve relative to the selected entry file. Changes to the entry file or local
Lua modules loaded through standard `require`, `hw.reload()` and SIGHUP request
a fresh invocation. Reload closes the current invocation and loads the
configuration from disk. Configuration errors release resources, print a
diagnostic and exit.

## Compose a shell

```lua
local hw = require("hyprworks")
local bar = assert(hw.layer {
    edge = "top", layer = "top", keyboard = "none", width = 0, height = 32, exclusive = true,
})

assert(bar:set_content {
    kind = "row", padding = 6, gap = 12, background = "#202630",
    children = {
        {kind = "text", id = "clock", font = "Sans 12", color = "#ffffff"},
        {kind = "button", text = "Exit", font = "Sans 12", color = "#ffffff",
            on_activate = function() hw.quit(0) end},
    },
})

local minute = hw.timer(0, function(timer)
    assert(bar:find("clock"):set {text = os.date("%H:%M")})
    timer:start((60 - os.time() % 60) * 1000)
end)
assert(hw.on_clock_change(function() minute:start(0) end))
```

Surfaces host element descriptions. `set_content` and `replace_children` commit
validated replacements; `{preserve_state = true}` retains matching explicit-ID
and kind focus, selection and scrolling. Explicit property values take
precedence. Reacquire element handles after structural changes.

Buttons activate on release of Space or Enter; toggles change on release of
Space. The originating seat, physical key and focused element own the press.
Focus changes, tree replacement, disabling and consumed key events cancel it.
`on_input` key events expose `repeated` for author-defined keyboard behavior.
Nested scroll containers consume movement on their axis and pass the remainder
to their ancestors. Their `on_change` callbacks report the final clamped offsets.

### Authored appearance and range controls

Descriptions supply `kind`; text and inputs supply a Pango `font` with family
and size. Colors, backgrounds, padding, borders, corner radii, focus outlines,
selection and caret painting are optional authored properties. Element dimensions
default to content and parent layout.
Windows and popups require integer `width` and `height`; layers additionally
require `edge`, `layer` and `keyboard`. A layer dimension of zero asks the
compositor to allocate that axis. Titles and application IDs are optional strings.

Inputs expose `selection_color`, `caret_color`, `caret_width` and
`placeholder_color`; focus outlines use `focus_color` and `focus_width`.
Password inputs require a one-character UTF-8 `mask`. Text clips at its bounds;
`wrap = true` enables wrapping, and `ellipsize = true` enables end ellipsis for
unwrapped text. Image source tables select `path`, then `icon`, then `data`;
each source reads its own options. Icon sources require `theme` and integer `pixels`: the icon's
intrinsic logical size. The runtime selects a raster size for the output scale.
SVG sources use intrinsic dimensions or an explicit `pixels` raster width.
For SVGs with percentage dimensions, `pixels` is required; a `viewBox` supplies
the aspect ratio, otherwise the authored size defines a square viewport.

Sliders and scroll containers require a boolean `horizontal`. A slider owns
normalized range state and positions its direct child marked `thumb = true`.
Its children are drawing/layout elements: boxes, rows, columns, text and images.
Other children draw the track or labels. `step` supplies the keyboard
increment; `jump = true` enables track-click positioning.
A scrollbar is a slider whose `scroll_target` names a scroll container:

```lua
assert(surface:set_content {
    kind = "row", gap = 8,
    children = {
        {kind = "scroll", id = "content", horizontal = false, grow = 1,
            children = {{kind = "box", height = 1200, background = "#202630"}}},
        {kind = "slider", id = "scrollbar", horizontal = false,
            scroll_target = "content", width = 12, jump = true, step = 0.05,
            background = "#141a22",
            children = {{kind = "box", thumb = true, background = "#8ab4f8", radius = 6}}},
    },
})
```

The target must be in a separate tree branch. Relationships are validated and
resolved at tree commit. With an omitted thumb length, a scrollbar derives its
length from the viewport/content ratio. An explicit length overrides that
calculation. Standalone sliders use authored thumb dimensions.
Scroll offsets remain owned by the scroll container; its `on_change` reports
coalesced changes. `element:state()` exposes `scroll_limit`, `pressed` and
`focused` as well as text, value, offset and geometry.

Drag capture belongs to the originating seat. A preserved slider keeps its grab
position while its target, axis and thumb remain compatible across tree edits.

Elements accept accessibility metadata:

```lua
{kind = "button", text = "×", font = "Sans 12", color = "#ffffff",
 accessibility = {role = "auto", name = "Close panel", description = "Close the active panel"}}
```

`accessibility` supports `name`, `description`, `hidden` and `role`. Roles are
`auto`, `group`, `label`, `image`, `button`, `checkbox`, `slider`, `textbox`,
`heading`, `link`, `list`, `list_item`, `menu` and `menu_item`. `auto` derives the
role from the element kind. Names and descriptions come from the authored metadata.
`element:accessibility()` returns effective metadata, ancestor-inherited
hidden state, enabled/focus state, and a toggle's checked or slider's value state.
Metadata updates use the same validated transactions and content budgets as
visual properties.

## Native state and operations

Observers such as `hw.audio()`, `hw.upower()` and `hw.hyprland.workspaces()`
publish snapshots. Read them in `on_change`; use `on_status` for connection
lifecycle and `on_result` for action completion. Each setter replaces one
callback, and `nil` removes it. Status/change subscriptions also schedule an
initial delivery.

With an `on_result` subscription, action results stay with their connection
binding until callback admission or `close()`. Closing can take an outstanding
result once, including from a finalizer that runs while callback arguments are
being built. Callback delivery checks the live handle, generation and registered
function after argument allocation.

Use native events to update state and timers for debounce, retry and display
deadlines. Lua callbacks run serially on the invocation thread. Operational
failures return `nil, error`; malformed arguments raise Lua errors.

Lua arguments retain their declared types: strings, booleans, integers and
numbers are checked at ingress, including optional values when present. Integer
fields require Lua integers. Optional `nil` means absence; in property updates, omitted fields
retain their current state. An input callback returns a boolean or `nil`.
Option and element tables use their stored fields. Element updates apply the
properties used by that kind; structural properties belong to tree descriptions.

`hw.notifications {name = ..., vendor = ..., version = ...}` supplies the
server identity. Optional `capabilities` are strings; optional `default_timeout`
is an integer number of milliseconds used when a sender delegates expiry.
Omit it or set it to zero to keep delegated notifications until dismissal.
Positive sender timeouts take precedence.
`notifications:close(id, reason)` requires the protocol close reason.

### D-Bus services

`hw.dbus {bus = "session"}` (or `"system"`) opens an asynchronous connection.
The C++ API is in [`include/hyprworks/dbus.hpp`](include/hyprworks/dbus.hpp).
Register `bus:on_event(function(bus, event) ... end)` before requesting a name.

- `bus:request_name(name)` starts acquisition of an available name.
  `name_acquired`, `name_failed` and `name_lost` events identify the name
  in `event.member`; failures include `error_name` and `error`.
- `bus:owned_names()` returns ownership acknowledged through these name operations.
  `bus:release_name(name)` completes asynchronously
  with `name_lost`; an operation already pending on that name must complete first.
  Closing the connection releases every name.
- `bus:export {path, interface, timeout, methods, signals, properties}` declares
  one interface. `timeout` is required (1–300000 ms); the three member lists are
  optional, with at least one member required. Introspection is generated from
  these declarations:

  ```lua
  assert(bus:export {
      path = "/org/example/Service", interface = "org.example.Service", timeout = 30000,
      methods = {{name = "Stop", input = "o", output = ""}},
      signals = {{name = "Stopped", signature = "o"}},
      properties = {{name = "Version", signature = "u", value = 1, constant = true}},
  })
  ```

- Incoming `method` events contain a unique request `id`, the caller's unique
  bus name in `sender`, the address and decoded `arguments`. The protocol owner
  is responsible for authorization. `bus:reply(id, signature, arguments)` or
  `bus:error(id, error_name, message)` consumes a request on success; invalid
  replies leave it available until its deadline.
- `bus:emit {path, interface, member, signature, arguments, destination}` sends
  a declared signal. Omit `destination` for broadcast. Arguments must match the
  declared signature.
- Properties implement standard `Get`/`GetAll` and are read-only to remote
  callers. `bus:set_property(path, interface, name, signature, value)` updates a
  nonconstant property and publishes `PropertiesChanged`. Type and retained
  storage limits are checked before replacement.
- `bus:unexport(path, interface)` removes just that interface and retires its
  pending requests with `UnknownObject`. Other interfaces and requests continue.
  Request IDs remain unique across re-export and connection restart.
- `expired` events include the request ID and reason for timeout, removal or
  caller disappearance. Further replies are rejected. `peer_gone` events carry
  a departed unique name in `sender`, including peers with no pending request,
  so native owners can retire longer-lived sessions. `bus:cancel(id)` cancels local
  reply tracking; an exported protocol method supplies remote cancellation.

Signature `h` uses owned descriptor userdata. `hw.dbus_fd(integer_fd)` duplicates
an existing descriptor with close-on-exec; received descriptors are also duplicated
out of their messages. `fd:fileno()` borrows the descriptor number; ownership
stays with the userdata. `fd:close()` releases that ownership. Garbage
collection and Lua `<close>` also release ownership. Queued messages and retained
properties own descriptor duplicates. Delivered descriptors remain valid
after the sender closes its userdata or connection. In C++, `FileDescriptor`
copies share ownership; `duplicate()` creates an independent close-on-exec FD.

Each connection bounds outgoing calls/subscriptions to 64 each, incoming requests
to 32, tracked names to 32, and exported interfaces to 128 (32 members of each
kind per interface). Bodies allow 8 MiB payload, 65536 values, depth 32 and 64 FDs.
Retained properties per interface allow 64 KiB payload, 4096 values and 16 FDs;
nesting also accounts for the standard property response containers. Native
events per dispatch are limited to 256 entries, 16 MiB and 256 FDs. Explicit
outgoing operations reject admission at 64 queued transport messages. Async
decoding/queue failures and uncertain name-operation timeouts fail the connection
and release its resources; observe `on_status` to discard associated state.

The [native lifecycle checks](tests/dbus_core.cpp) and
[Lua service fixture](tests/dbus_service.lua) run on private buses and cover
ownership, cancellation, request expiry and FD lifetimes.

### ScreenCast portal

`HYPRWORKS_BUILD_PORTAL` enables ScreenCast and requires GUI, Lua, D-Bus and
PipeWire 1.4 or newer. Save the persistent entry as `portal-service.lua`:

```lua
local hw = require("hyprworks")
assert(hw.portal {max_fps = 30})
```

Provide the selector in `portal-ui.lua` alongside it in `HYPRWORKS_CONFIG_DIR`,
then enable the packaged backend service:

```sh
systemctl --user daemon-reload
systemctl --user enable --now hyprworks-portal.service
```

Select the backend in the desktop's `xdg-desktop-portal` configuration:

```ini
[preferred]
org.freedesktop.impl.portal.ScreenCast=hyprworks;
```

Restart `xdg-desktop-portal.service` to apply routing changes. D-Bus starts the
selector on demand.

The backend owns `org.freedesktop.impl.portal.desktop.hyprworks` and implements
`org.freedesktop.impl.portal.ScreenCast` version 3:
monitor and window selection, single or multiple sources, and hidden or
embedded cursors. `xdg-desktop-portal` owns the application-facing API and
returns a permission-scoped PipeWire remote to applications. The backend
accepts protocol methods from that frontend's current unique bus owner.
Sessions use explicit selection for each start.

The authored selector owns `org.hyprworks.PortalUI`. Its control interface is
`org.hyprworks.Portal` at `/org/freedesktop/portal/desktop`:

| Method | Input → output |
|---|---|
| `Attach` | `""` → `taa{sv}aa{sv}`: source revision, sessions, sources |
| `Snapshot` | `""` → the same snapshot, after attaching |
| `Choose` | `ttas`: session ID, source revision, source IDs |
| `Cancel` | `t`: pending session ID |
| `Stop` | `t`: session ID |

Session dictionaries contain `id` (`t`), `app_id`, `parent_window`, `phase`,
`types` and `multiple`. Source dictionaries contain `id`, `title` and `type`.
`Changed` schedules a fresh snapshot; `Failed(ts)` identifies a session and its
failure message. Signals target the attached selector. Only the current owner
of the UI name can attach or submit decisions. Session IDs survive selector
reconnection; source revisions reject obsolete selections.

`Request.Close`, `Session.Close`, frontend disappearance, source removal and
invocation shutdown close sessions. Pending interactions expire after three
minutes; stream startup has a ten-second deadline.

There are eight sessions and eight capture slots per invocation, with up to
four sources per selection. Delivered stream dimensions fit within 4096×4096;
capture buffers use the capture subsystem's 64 MiB per-buffer and 128 MiB total
limits. Negotiated PipeWire buffers share a 256 MiB budget, with two buffers per
stream. `max_fps` accepts 1–60. Streams deliver BGRA frames at that rate and
renegotiate their dimensions when the source resizes.

### Interactive children

`hw.process(argv, options)` starts a child with nonblocking parent-side pipes.
`timeout` (1 ms–24 h) and `output_limit` (0–16 MiB **per captured stream**) are
required. Exceeding the output limit ends the job with a truncated result.
`directory` selects the working directory; `env = {KEY = "value"}` overrides
the child's inherited environment, including `PATH`.

```lua
local p = assert(hw.process({"cat"}, {timeout = 5000, output_limit = 65536}))
p:on_output(function(_, stream, bytes) print(stream, #bytes) end)
assert(p:write("one\0two") == 7)
assert(p:close_input()) -- send pipe EOF after queued bytes have been written
```

- `write(bytes)` returns the number **queued**, possibly zero. Retry the
  unaccepted suffix in `on_drain(function(p, discarded) ... end)`. `input_limit`
  defaults to 64 KiB and can be set to 1 byte–1 MiB. `discarded` is zero on a
  successful drain, or the unsent byte count if the child closed its input.
- `on_output(p, "stdout"|"stderr", bytes)` delivers binary chunks as they arrive,
  before `on_exit`. `read_output(stream, offset, limit)` returns a slice of the
  retained stream (zero-based offset, up to 64 KiB).
- `stdin_file` redirects input from a path; `stdout_file` and `stderr_file`
  send output directly to **new**, mode-0600 files. Paths are relative to the
  child's working directory. The capture limit applies to pipe and PTY output.
  A spawn failure can leave a created output file.
- Set `pty = {rows = 24, columns = 80, raw = false, echo = true}` to start a
  new session with a controlling terminal for all three streams. Output arrives
  on `stdout`; file redirections apply in pipe mode.
  `pty = {}` uses those defaults; raw mode defaults to echo off. `resize(rows,
  columns)` updates the terminal; `signal(number)` targets its foreground
  process group (the child's group in pipe mode). For canonical terminal EOF,
  send the configured EOF character (normally `"\4"`). `hangup()` closes the
  master immediately and continues watching
  the child until exit or timeout. Timeout, `close()` and invocation shutdown
  reap the owned child.

For work that outlives a window or a GUI process, run the owning entry as an
independent user service: `hyprworks --headless --no-watch --entry updates.lua`.
The entry owns the child, its lock and its session-bus name. It defines what a
returning client can read or submit. `--no-watch` keeps the invocation running
across edits to the configuration files.

### Clipboard and files

Save `event.seat` from a surface input or input-driven element callback. Use
that ID even if the copy or paste happens later. The seat needs an input serial
for a write; the compositor decides whether to grant the selection.

```lua
assert(hw.clipboard_write(seat, {{mime = "image/png", image = capture_event.source}}))
-- Other representations: {mime = "...", file = path} or {mime = "...", data = bytes}.
local types = assert(hw.clipboard_types(seat))
assert(hw.clipboard_read(seat, types[1], {file = destination, limit = 32 * 1024 * 1024},
    function(count, err) assert(count, err) end))
```

One write publishes up to 16 MIME representations. Captured frames encode as
PNG; inline data is binary (16 MiB total). File offers pin an open regular
file up to 256 MiB: unlinking or renaming it is safe; leave its contents
unchanged. The seat serves the selection and in-flight transfers after the
preview closes, for the lifetime of the Wayland invocation.
Omit `file` on a read to receive a byte string (up to 16 MiB); file reads
stream to a new destination (up to 256 MiB), published only at complete EOF.
Errors are delivered as `callback(nil, reason)`.

`hw.temp_file(parent)` and `hw.temp_dir(parent)` securely create a private file
or directory and return its path; the caller removes it. `hw.mkdir(path)`
creates one directory. `hw.create_file(path, bytes)` and `hw.copy_file(from,
to, byte_limit)` require a new destination.
`hw.write_file(path, bytes)` atomically replaces it.

For existing preferences use `hw.update_file(path, limit, edit)`. It takes
an exclusive, nonblocking lock on `path .. ".lock"` **before reading**, passes
the current bytes to `edit`, and keeps the lock through comparison and atomic
replacement. Return new bytes to commit, or `nil` to skip. The result is
`true` for a change, `false` for no change, or `nil, error` on operational
failure. An error raised by `edit` preserves the original file and releases the lock.
For a longer session, `local lock <close> = assert(hw.lock_file(lock_path))`
holds the lock until scope exit or `lock:close()`. All participants must use
the same lock path; leave the lock file in place between acquisitions.

### Polkit authentication

`hw.polkit()` opens the login session's authentication agent. Hyprworks verifies
the authority and owns the helper and retries; Lua supplies the prompt and the
user's choice of identity. Build with `HYPRWORKS_BUILD_POLKIT=ON`.

```lua
local agent = assert(hw.polkit()) -- optional {session = "1", locale = "en_US.UTF-8"}
agent:on_status(function(handle)
    local status = handle:status()
    if status.phase == "failed" then show_agent_error(status.error) end
end)
agent:on_event(function(handle, event)
    if event.kind == "request" then
        show_authentication(event.message, event.icon, event.identities,
            function(uid) assert(handle:authenticate(event.id, uid)) end,
            function() assert(handle:cancel(event.id)) end)
    elseif event.kind == "prompt" then
        show_prompt(event.text, event.echo, function(response)
            assert(handle:respond(event.id, event.prompt, response))
        end)
    elseif event.kind == "info" or event.kind == "error" then
        show_authentication_message(event.text)
    elseif event.kind == "completed" then
        hide_authentication(event.authorized, event.cancelled)
    end
end)
```

Request and prompt IDs are opaque decimal strings. Choose one of the offered
`{uid, name}` identities. Each response belongs to one prompt and accepts up
to 4095 bytes of single-line text (NUL, CR and LF are invalid). Mask input
when `echo` is false and clear the editor after submission or dismissal.
`completed` reports authentication; Polkit returns the authorization decision
to the requesting application.

The agent admits one request at a time, with five seconds for registration
and five minutes per request including identity selection and retries.
`cancel(id)` dismisses it. `close()` releases the registration and callbacks;
`restart()` registers with the original options.

### Workspace captures

`hw.capabilities().workspace_capture` reports native workspace-capture support.
In a GUI invocation, capture a normal workspace by its display name and output:

```lua
local capture = assert(hw.hyprland.capture_workspace({
    name = "1", output = "DP-1", mode = "everything",
    max_width = 320, max_height = 180, max_fps = 1,
}, function(handle, event)
    if event.kind == "frame" then
        preview:set {source = event.source}
    else
        handle:close()
    end
end))
```

The compositor renders active and inactive workspaces. Required `mode = "windows_only"`
includes windows and decorations over transparency; `"everything"`
also includes the output's wallpaper, panels, pinned windows and layer surfaces.
This includes preview UI on that output unless the compositor excludes it.
Matching requires a unique workspace name on the selected output. The capture
keeps the native workspace identity across renames and moves.

Frames use the same immutable image sources and event fields as window captures:
`source`, `width`, `height`, `source_width`, `source_height`. Terminal events are
`"unavailable"` or `"error"` with an `error` string. `capture:close()` cancels
delivery and releases the session; already delivered image sources remain valid.
Workspace removal ends capture with `"unavailable"`. Workspace captures remain
active across window-catalog restarts and closure.

Window and workspace captures share eight active slots and 128 MiB of capture
buffers per invocation, with a 64 MiB per-buffer limit. Required integer
`max_width` and `max_height` are bounded to 1–1024; required `max_fps` accepts 1–30.
Frames preserve aspect ratio and are downscaled after capture: these dimensions
bound delivered images, while compositor buffers retain their native size.
Retained frame sources share the image subsystem's separate 128 MiB budget.
Release captures when their previews leave the viewport.

This requires `ext-workspace-v1`, `hyprland-workspace-image-capture-source-v1`
and `ext-image-copy-capture-v1`, as provided by Hyprland 0.57.2.

## Develop and verify

See [ARCHITECTURE.md](ARCHITECTURE.md) for resource ownership and execution
order. CTest runs the core lifetime checks and the isolated D-Bus fixtures for
the enabled features. Run these Wayland fixtures in a Hyprland session:

```sh
HYPRWORKS_CONFIG_DIR="$PWD/tests" build/local/hyprworks --entry gui.lua
HYPRWORKS_CONFIG_DIR="$PWD/tests" build/local/hyprworks --entry windows.lua
HYPRWORKS_CONFIG_DIR="$PWD/tests" build/local/hyprworks --entry workspace_capture.lua
HYPRWORKS_CONFIG_DIR="$PWD/tests" dbus-run-session -- sh tests/portal.sh \
  build/local/hyprworks build/local/hyprworks-pipewire-capture
dbus-run-session -- sh tests/portal_public.sh \
  build/local/hyprworks build/local/hyprworks-pipewire-capture
```

Portal fixtures use a private D-Bus session and the running Wayland/PipeWire
servers. The public fixture runs `/usr/lib/xdg-desktop-portal`, exercises its
application-facing API and consumes the returned permission-scoped remote.
The backend fixture verifies selector reconnection, cancellation and session
cleanup with clients supplied in this repository.

With the existing session agent stopped, `tests/polkit.lua` checks real Polkit
registration, PAM prompt delivery and cancellation at the password prompt
using `pkcheck`. Restore the existing agent afterward. `polkit-core`
runs on an isolated bus with controlled helper signals to check retries,
caller ownership and cancellation races.

For sanitizer builds, pass compiler and linker sanitizer flags through CMake.
The `image-worker` CTest target runs independently of font initialization.

## Embed and install

Use `find_package(hyprworks CONFIG REQUIRED)` and link `hyprworks::hyprworks`.
For Lua execution, request `COMPONENTS lua` and link `hyprworks::lua`.
Both targets support `add_subdirectory`.

An embedder owns `hyprworks::lua::Invocation`, calls `load()` and `run()`, and
keeps it alive through cross-thread quit/reload requests. Native-only hosts
drive each connection's `prepare_wait()` and `dispatch()` themselves.

Set `CMAKE_INSTALL_PREFIX`, then use `cmake --install`. Packages stage the same
install with `DESTDIR`. GUI builds install user services for enabled features:

| Service | Entry | Startup |
|---|---|---|
| `hyprworks.service` | `main.lua` | Enable for the graphical session |
| `hyprworks-polkit.service` | `polkit-agent.lua` | Enable for the graphical session |
| `hyprworks-portal.service` | `portal-service.lua` | Enable for the graphical session |
| `hyprworks-portal-ui.service` | `portal-ui.lua` | D-Bus activation |

Units use the installed binary and read `HYPRWORKS_CONFIG_DIR` from the user
manager's environment. Shell, Polkit and selector units support reload.
The portal owner runs persistently. Portal builds also install the
`hyprworks.portal` descriptor and backend/selector D-Bus activation files.
