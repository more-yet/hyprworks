#pragma once

#include <hyprworks/hyprland/workspaces.hpp>

#include <cstddef>
#include <string_view>

namespace hyprworks::hyprland::protocol {

inline constexpr std::string_view snapshot_request = "[[BATCH]]j/workspaces;j/monitors all";
inline constexpr std::size_t max_snapshot_bytes = 1024 * 1024;
inline constexpr std::size_t max_command_bytes = 4096;
inline constexpr std::size_t max_switch_reply_bytes = 64 * 1024;
inline constexpr std::size_t max_event_bytes = 4096;
inline constexpr std::size_t io_budget = 64 * 1024;

std::expected<bool, Error> connect_socket(int& fd, const std::string& path);
std::expected<SocketPhase, Error> advance_request(int fd, short events, SocketPhase phase, std::string_view request,
    std::string& reply, std::size_t limit, bool& wrote);

std::expected<Snapshot, Error> parse_snapshot(std::string_view reply);
std::expected<std::string, Error> encode_switch(const Workspace& workspace);
bool invalidates_workspaces(std::string_view line);

}
