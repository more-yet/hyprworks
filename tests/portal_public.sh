#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export HYPRWORKS_CONFIG_DIR="$root/tests"
directory=$(mktemp -d "${TMPDIR:-/tmp}/hyprworks-public.XXXXXX")
cp "$root/share/hyprworks.portal" "$root/tests/portal-public/xdg-desktop-portal/portals.conf" "$directory"
"$1" --no-watch --entry portal_service.lua &
service=$!
ui=
frontend=
cleanup() {
    status=$?
    kill "$service" ${ui:+"$ui"} ${frontend:+"$frontend"} 2>/dev/null || :
    wait || :
    if test "$status" != 0 && test -f "$directory/frontend.log"; then cat "$directory/frontend.log"; fi
    rm -rf -- "$directory"
}
trap cleanup EXIT
gdbus wait --session --timeout 5 org.freedesktop.impl.portal.desktop.hyprworks
"$1" --headless --no-watch --entry portal_public_ui.lua &
ui=$!
gdbus wait --session --timeout 5 org.hyprworks.PortalUI
XDG_DESKTOP_PORTAL_DIR="$directory" \
    /usr/lib/xdg-desktop-portal --verbose >"$directory/frontend.log" 2>&1 &
frontend=$!
gdbus wait --session --timeout 10 org.freedesktop.portal.Desktop
"$2" --portal
"$2" --portal-monitor
