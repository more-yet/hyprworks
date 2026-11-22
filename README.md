# Hyprworks

A C++ runtime for building Hyprland desktop shells in Lua.

Lua owns desktop composition, appearance and interaction policy. Hyprworks owns
native connections, element state, input, rendering and asynchronous work.
The runtime brings those resources together in one invocation and event loop.

## Build and run

Use a C++23 compiler, CMake, Ninja and pkg-config. The build resolves system
libraries for Wayland, Cairo/Pango, image decoding, Lua, json-c, sd-bus, GIO and
libpulse. Dependency versions and feature options live in [CMakeLists.txt](CMakeLists.txt).

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DHYPRWORKS_BUILD_TESTS=ON
cmake --build build/local
ctest --test-dir build/local --output-on-failure
build/local/hyprworks --config /path/to/main.lua
```

`HYPRWORKS_BUILD_GUI=OFF` builds a headless runtime.
`HYPRWORKS_BUILD_LUA=OFF` builds the native library.
Desktop integrations have individual `HYPRWORKS_BUILD_<FEATURE>` options.

The CLI loads `~/.config/hyprworks/main.lua`, respecting `XDG_CONFIG_HOME`.
Modules resolve relative to the entry file. Changes to the entry file or local
Lua modules loaded through standard `require`, `hw.reload()` and SIGHUP request
a fresh invocation. Reload closes the current invocation and loads the
configuration from disk. Configuration errors release resources, print a
diagnostic and exit.

## Compose a shell

```lua
local hw = require("hyprworks")
local bar = assert(hw.layer {edge = "top", height = 32, exclusive = true})

assert(bar:set_content {
    kind = "row", padding = 6, gap = 12,
    children = {
        {kind = "text", id = "clock"},
        {kind = "button", text = "Exit", on_activate = function() hw.quit() end},
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

Elements accept accessibility metadata:

```lua
{kind = "button", text = "×",
 accessibility = {name = "Close panel", description = "Close the active panel"}}
```

`accessibility` supports `name`, `description`, `hidden` and `role`. Roles are
`auto`, `group`, `label`, `image`, `button`, `checkbox`, `slider`, `textbox`,
`heading`, `link`, `list`, `list_item`, `menu` and `menu_item`. `auto` derives the
role from the element kind. Names default to control labels or input placeholders.
`element:accessibility()` returns effective metadata, ancestor-inherited
hidden state, enabled/focus state, and a toggle's checked or slider's value state.
Metadata updates use the same validated transactions and content budgets as
visual properties.

Observers such as `hw.audio()`, `hw.upower()` and `hw.hyprland.workspaces()`
publish snapshots. Read them in `on_change`; use `on_status` for connection
lifecycle and `on_result` for action completion. Each setter replaces one
callback, and `nil` removes it. Status/change subscriptions also schedule an
initial delivery.

Use native events to update state and timers for debounce, retry and display
deadlines. Lua callbacks run serially on the invocation thread. Operational
failures return `nil, error`; malformed arguments raise Lua errors.

The [live fixtures](tests/) demonstrate surface lifecycle, window capture and
asynchronous D-Bus request ownership.

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

The compositor renders active and inactive workspaces. `mode = "windows_only"`
(the default) includes windows and decorations over transparency; `"everything"`
also includes the output's wallpaper, panels, pinned windows and layer surfaces.
This includes preview UI on that output unless the compositor excludes it.
Matching uses the workspace's current name and output, rejects ambiguous names,
and then retains the native workspace identity across renames and moves.

Frames use the same immutable image sources and event fields as window captures:
`source`, `width`, `height`, `source_width`, `source_height`. Terminal events are
`"unavailable"` or `"error"` with an `error` string. `capture:close()` cancels
delivery and releases the session; already delivered image sources remain valid.
Workspace removal ends capture with `"unavailable"`. Workspace captures remain
active across window-catalog restarts and closure.

Window and workspace captures share eight active slots and 128 MiB of capture
buffers per invocation, with a 64 MiB per-buffer limit. Dimensions default to
448×252 and are bounded to 1–1024; `max_fps` defaults to 1 and accepts 1–30.
Frames preserve aspect ratio and are downscaled after capture: these dimensions
bound delivered images, while compositor buffers retain their native size.
Retained frame sources share the image subsystem's separate 128 MiB budget.
Release captures when their previews leave the viewport.

This requires `ext-workspace-v1`, `hyprland-workspace-image-capture-source-v1`
and `ext-image-copy-capture-v1`, as provided by Hyprland 0.57.2.

## Find the code

| Path | Responsibility |
|---|---|
| [`include/hyprworks/`](include/hyprworks/) | Public native and embedding contracts |
| [`src/native/`](src/native/) | Protocol connections, snapshots, requests and OS resources |
| [`src/lua/binding.hpp`](src/lua/binding.hpp) | Lua error boundary, registry references and protected marshalling |
| [`src/runtime/invocation.cpp`](src/runtime/invocation.cpp) | Configuration lifetime, readiness collection and callback delivery |
| [`src/lua/`](src/lua/) | Lua argument translation and integration bindings |
| [`src/gui/gui.cpp`](src/gui/gui.cpp) | Display lifetime, readiness and ordered GUI dispatch |
| [`src/gui/surface.cpp`](src/gui/surface.cpp) | Surface roles, transactional element trees and Lua handles |
| [`src/gui/input.cpp`](src/gui/input.cpp) | Seat events, editing, clipboard transfers and input delivery |
| [`src/gui/render.cpp`](src/gui/render.cpp) | Text geometry, layout, painting and presentation buffers |
| [`src/gui/images.cpp`](src/gui/images.cpp) and [`icons.cpp`](src/gui/icons.cpp) | Image worker, storage accounting and icon lookup |
| [`src/gui/windows.cpp`](src/gui/windows.cpp) | Synchronized window observation and shared window/workspace capture |
| [`src/runner.cpp`](src/runner.cpp) | CLI configuration, process signals and reload loop |
| [`protocols/`](protocols/) and [`cmake/`](cmake/) | Wayland definitions, installed targets and user service |

Read [ARCHITECTURE.md](ARCHITECTURE.md) for end-to-end execution paths,
failure/cancellation branches, ownership and simplification tradeoffs.

## Develop and verify

Follow a change from its input through native state, publication, Lua delivery
and cleanup. Keep protocol validation with the connection, Lua conversion with
the binding, and UI interaction state with the surface. Validate at ingress and
mutation boundaries; consumers rely on the resulting invariants. Revalidate
identities across callbacks, Lua allocation and asynchronous completion.

The core checks exercise invocation teardown, callback-driven mutation,
transactional replacement, clipboard ownership and image-worker synchronization.
Live checks run against their corresponding desktop services:

```sh
build/local/hyprworks --config tests/gui.lua
build/local/hyprworks --config tests/windows.lua
build/local/hyprworks --config tests/workspace_capture.lua
dbus-run-session -- build/local/hyprworks --headless --config tests/dbus.lua
```

For sanitizer builds, pass compiler and linker sanitizer flags through CMake.
The `image-worker` CTest target runs independently of font initialization.

## Embed and install

Use `find_package(hyprworks CONFIG REQUIRED)` and link `hyprworks::hyprworks`.
For Lua execution, request `COMPONENTS lua` and link `hyprworks::lua`.
Both targets support `add_subdirectory`.

An embedder owns `hyprworks::lua::Invocation`, calls `load()` and `run()`, and
keeps it alive through cross-thread quit/reload requests. Native-only hosts
drive each connection's `prepare_wait()` and `dispatch()` themselves.

Set `CMAKE_INSTALL_PREFIX` for installation, then use `cmake --install`.
Packages stage the same install with `DESTDIR`. The installed
`hyprworks.service` follows the graphical session and supports
`systemctl --user reload hyprworks.service`.
