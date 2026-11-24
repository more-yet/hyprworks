#!/bin/sh
set -eu
"$1" --no-watch --entry portal_service.lua &
service=$!
trap 'kill "$service" 2>/dev/null || :; wait "$service" || :' EXIT
gdbus wait --session --timeout 5 org.freedesktop.impl.portal.desktop.hyprworks
HYPRWORKS_PIPEWIRE_CHECK="$2" "$1" --headless --no-watch --entry portal_client.lua
