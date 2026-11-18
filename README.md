# Hyprworks

Build desktop shells for Hyprland in Lua. Create bars, docks, launchers and widgets
with support for workspaces, audio, media, notifications, tray menus, power and
desktop entries.

## Getting started

Create `~/.config/hyprworks/main.lua`:

```lua
local hw = require("hyprworks")
local bar = hw.ui.layer {
    height = 32,
    exclusive_zone = 32,
    anchors = {top = true, left = true, right = true},
}
bar:set_content {kind = "text", text = "Hyprworks", halign = "center", valign = "center"}
```

Run your shell with `hyprworks`. Changes to `main.lua` automatically reload your shell.

Keep your Lua modules and themes beside `main.lua`. Hyprworks respects
`XDG_CONFIG_HOME`; use `hyprworks --config /path/to/main.lua` to select another configuration.
See `hyprworks --help` for command-line options.

## Styling

Use `hw.ui.set_palette(table)` to set colors, fonts, rounding and the icon theme.
Apply the palette before creating content. When switching themes, apply the new
palette and rebuild your content.

Buttons accept `children` for icon and text layouts. Update a container with
`surface:set_children(id, children)`.

## Starting with your session

Enable the user service after creating your configuration:

```sh
systemctl --user enable --now hyprworks.service
```

The service follows your graphical session. UWSM supplies the required
`WAYLAND_DISPLAY` and `HYPRLAND_INSTANCE_SIGNATURE` environment variables.
Other session setups should export them to the user service manager.

Reload your shell or view its logs:

```sh
systemctl --user reload hyprworks.service
journalctl --user -u hyprworks.service
```

Lua configurations can also reload with `hw.reload()`.

## Building from source

Requires a C++23 compiler, CMake 4.5+, Ninja, pkg-config and these dependencies:

| Component | Dependency |
|---|---|
| Core | json-c 0.19+, Lua 5.5.1+ |
| GUI | Patched hyprtoolkit/hyprgraphics 0.6.0, xkbcommon 1.13.2+ |
| D-Bus integrations | libsystemd 262+ |
| Audio | libpulse 17+ |
| Global shortcuts | wayland-client and wayland-scanner 1.26+ |
| Desktop entries | gio-unix-2.0 2.90.1+ |

### Dependency patches

Use the `v0.6.0` tags of hyprgraphics and hyprtoolkit. Each patch applies directly
to its upstream tag:

- hyprgraphics: `patches/hyprgraphics-0.6.0-resource-lifetime.patch`
- hyprtoolkit: `patches/hyprtoolkit-0.6.0-shell.patch`

Place the patched source trees in `build/deps/hyprgraphics` and
`build/deps/hyprtoolkit`. From the Hyprworks directory, build and install them into
a private prefix:

```sh
for library in hyprgraphics hyprtoolkit; do
  PKG_CONFIG_PATH="$PWD/build/deps/prefix/lib/pkgconfig" \
    cmake -S "build/deps/$library" -B "build/deps/$library/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
      -DCMAKE_INSTALL_PREFIX="$PWD/build/deps/prefix" -DCMAKE_INSTALL_LIBDIR=lib \
      '-DCMAKE_INSTALL_RPATH=$ORIGIN' || break
  cmake --build "build/deps/$library/build" --target "$library" || break
  cmake --install "build/deps/$library/build" || break
done
```

These dependencies also require a C++26-capable compiler, hyprwayland-scanner and
wayland-protocols.

### Build and run

```sh
PKG_CONFIG_PATH="$PWD/build/deps/prefix/lib/pkgconfig" cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build/local
build/local/hyprworks
```

For packaging, stage the installation with `DESTDIR="$pkgdir" cmake --install build/local`.
The installation includes the patched graphics libraries under `lib/hyprworks`
(`/usr/lib/hyprworks` with the prefix above), using their normal filenames.
Hyprworks finds them through embedded runtime paths; keep the dependency build's
`$ORIGIN` setting so hyprtoolkit also finds its adjacent hyprgraphics copy.
Dependency headers and pkg-config files stay in the development prefix.

### C++ embedding

Use `find_package(hyprworks CONFIG REQUIRED)` and link `hyprworks::hyprworks`.
For Lua embedding, request `COMPONENTS lua` and link `hyprworks::lua`.
Both targets also support `add_subdirectory`.
