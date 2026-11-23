#!/bin/sh
set -eu
"$1" --headless --no-watch --entry job_owner.lua &
owner=$!
trap 'kill "$owner" 2>/dev/null || :; wait "$owner" || :' EXIT
name=org.hyprworks.TestTerminal
gdbus wait --session --timeout 3 "$name"
call() { busctl --user --timeout=3 -- call "$name" /terminal "$name" "$@"; }

# Each call's bus connection closes before the next client starts.
call Answer s first
test "$(call Finish s second)" = 's "first:second:30 90"'
