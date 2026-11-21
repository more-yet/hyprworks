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
| [`src/gui/windows.cpp`](src/gui/windows.cpp) | Synchronized window observation and capture |
| [`src/runner.cpp`](src/runner.cpp) | CLI configuration, process signals and reload loop |
| [`protocols/`](protocols/) and [`cmake/`](cmake/) | Wayland definitions, installed targets and user service |

Read [ARCHITECTURE.md](ARCHITECTURE.md) for ownership, execution order,
resource lifetimes and the invariants shared across these paths.

## Develop and verify

Follow a change from its input through native state, publication, Lua delivery
and cleanup. Keep protocol validation with the connection, Lua conversion with
the binding, and UI interaction state with the surface. Add checks where
ownership, cancellation, concurrency or transactional integrity can change.

The core checks exercise invocation teardown, callback-driven mutation,
transactional replacement, clipboard ownership and image-worker synchronization.
Live checks run against their corresponding desktop services:

```sh
build/local/hyprworks --config tests/gui.lua
build/local/hyprworks --config tests/windows.lua
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
