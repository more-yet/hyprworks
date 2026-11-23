#include <hyprworks/hyprland/workspaces.hpp>
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace hyprworks::hyprland {
namespace {

std::expected<void, Error> validate_target(const SessionTarget& target) {
    if (target.directory.empty() || target.directory.front() != '/' || target.directory.contains('\0'))
        return std::unexpected(Error{ErrorCode::invalid_target, "session", "session directory must be an absolute path without NUL"});
    if (target.directory.size() + std::string_view("/.socket2.sock").size() >= sizeof(sockaddr_un::sun_path))
        return std::unexpected(Error{ErrorCode::invalid_target, "session", "UNIX socket path is too long"});
    return {};
}

bool running(ConnectionPhase phase) {
    return phase == ConnectionPhase::connecting || phase == ConnectionPhase::synchronizing || phase == ConnectionPhase::ready;
}

}

std::expected<SessionTarget, Error> session_from_environment() {
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    const auto* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !*runtime || !signature || !*signature)
        return std::unexpected(Error{ErrorCode::unavailable, "session", "XDG_RUNTIME_DIR and HYPRLAND_INSTANCE_SIGNATURE are required"});
    const std::string_view component(signature);
    if (component == "." || component == ".." || component.contains('/'))
        return std::unexpected(Error{ErrorCode::invalid_target, "session", "invalid Hyprland instance signature"});
    SessionTarget target{std::string(runtime) + "/hypr/" + signature};
    if (auto valid = validate_target(target); !valid)
        return std::unexpected(valid.error());
    return target;
}

WorkspaceConnection::Socket::~Socket() { reset(); }

void WorkspaceConnection::Socket::reset() noexcept {
    if (fd >= 0)
        ::close(std::exchange(fd, -1));
}

WorkspaceConnection::WorkspaceConnection(ConnectionOptions options) : options_(options) {
    constexpr auto maximum = std::chrono::hours(24);
    if (options.request_timeout <= std::chrono::milliseconds::zero() || options.request_timeout > maximum ||
        options.event_coalesce < std::chrono::milliseconds::zero() || options.event_coalesce > maximum)
        throw std::invalid_argument("connection durations must be positive (coalescing may be zero) and at most 24 hours");
}

WorkspaceConnection::~WorkspaceConnection() = default;

std::expected<void, Error> WorkspaceConnection::start(SessionTarget target, TimePoint now) {
    if (running(status_.phase))
        return std::unexpected(Error{ErrorCode::busy, "start", "connection is already running"});
    if (auto valid = validate_target(target); !valid)
        return valid;
    stop();
    target_ = std::move(target);
    snapshot_.reset();
    subscription_marker_ = "hyprworks-ready:" + std::to_string(getpid()) + ":" +
                           std::to_string(now.time_since_epoch().count()) + ":" + std::to_string(wait_generation_);
    auto connected = protocol::connect_socket(events_.fd, target_.directory + "/.socket2.sock");
    if (!connected) {
        status_ = {ConnectionPhase::failed, false, connected.error()};
        return std::unexpected(connected.error());
    }
    event_connecting_ = !*connected;
    event_deadline_ = now + options_.request_timeout;
    marker_retry_at_ = now;
    status_.phase = event_connecting_ ? ConnectionPhase::connecting : ConnectionPhase::synchronizing;
    snapshot_before_switch_ = true;
    schedule_refresh(now);
    return {};
}

std::optional<SwitchResult> WorkspaceConnection::stop() {
    ++wait_generation_;
    events_.reset();
    close_command();
    event_line_.clear();
    refresh_at_.reset();
    event_connecting_ = false;
    subscription_ready_ = false;
    status_ = {};
    std::optional<SwitchResult> result;
    if (pending_switch_) {
        result = SwitchResult{pending_switch_->id, SwitchOutcome::cancelled, pending_switch_->may_have_executed, {}};
        pending_switch_.reset();
    }
    return result;
}

void WorkspaceConnection::schedule_refresh(TimePoint when) {
    if (!refresh_at_ || when < *refresh_at_)
        refresh_at_ = when;
    status_.refresh_pending = true;
}

std::expected<void, Error> WorkspaceConnection::refresh(TimePoint now) {
    if (!running(status_.phase))
        return std::unexpected(Error{ErrorCode::not_ready, "refresh", "connection is not running"});
    ++wait_generation_;
    schedule_refresh(now);
    return {};
}

std::expected<std::uint64_t, Error> WorkspaceConnection::switch_workspace(const WorkspaceKey& key, TimePoint now) {
    if (status_.phase != ConnectionPhase::ready)
        return std::unexpected(Error{ErrorCode::not_ready, "switch", "connection is not ready"});
    if (pending_switch_)
        return std::unexpected(Error{ErrorCode::busy, "switch", "a switch is already outstanding"});
    const auto workspace = std::ranges::lower_bound(snapshot_->workspaces, key, {}, &Workspace::key);
    if (workspace == snapshot_->workspaces.end() || workspace->key != key)
        return std::unexpected(Error{ErrorCode::unknown_workspace, "switch", "workspace is not in the published snapshot"});
    auto command = protocol::encode_switch(*workspace);
    if (!command)
        return std::unexpected(command.error());
    if (last_request_id_ == UINT64_MAX)
        return std::unexpected(Error{ErrorCode::limit_exceeded, "switch", "request identifiers exhausted"});
    pending_switch_ = PendingSwitch{++last_request_id_, key, std::move(*command), now + options_.request_timeout};
    ++wait_generation_;
    return last_request_id_;
}

WaitSet WorkspaceConnection::prepare_wait() const {
    WaitSet wait;
    wait.generation = wait_generation_;
    auto deadline = [&](TimePoint at) {
        if (!wait.deadline || at < *wait.deadline)
            wait.deadline = at;
    };
    if (!running(status_.phase))
        return wait;
    wait.fds[0] = {events_.fd, static_cast<short>(event_connecting_ ? POLLOUT : POLLIN), 0};
    if (!subscription_ready_)
        deadline(event_deadline_);
    if (command_phase_ != CommandPhase::idle) {
        wait.fds[1] = {command_.fd, static_cast<short>(command_phase_ == CommandPhase::reading ? POLLIN : POLLOUT), 0};
        deadline(command_deadline_);
    } else if (!event_connecting_) {
        if (!subscription_ready_) {
            deadline(marker_retry_at_);
            return wait;
        }
        if (pending_switch_)
            deadline(TimePoint::min());
        if (refresh_at_)
            deadline(*refresh_at_);
    }
    if (pending_switch_)
        deadline(pending_switch_->deadline);
    return wait;
}

void WorkspaceConnection::close_command() noexcept {
    command_.reset();
    command_phase_ = CommandPhase::idle;
    request_.clear();
    reply_.clear();
}

void WorkspaceConnection::complete_switch(SwitchOutcome outcome, std::optional<Error> error, DispatchResult& result) {
    result.switch_result = SwitchResult{pending_switch_->id, outcome, pending_switch_->may_have_executed, std::move(error)};
    pending_switch_.reset();
}

void WorkspaceConnection::fail_connection(Error error, ConnectionPhase phase, DispatchResult& result) {
    events_.reset();
    close_command();
    event_line_.clear();
    refresh_at_.reset();
    status_ = {phase, false, std::move(error)};
    if (pending_switch_)
        complete_switch(SwitchOutcome::failed, status_.error, result);
}

void WorkspaceConnection::fail_command(Error error, TimePoint now, DispatchResult& result) {
    if (command_kind_ != CommandKind::switch_workspace) {
        fail_connection(std::move(error), ConnectionPhase::failed, result);
        return;
    }
    close_command();
    complete_switch(SwitchOutcome::failed, std::move(error), result);
    snapshot_before_switch_ = true;
    schedule_refresh(now);
}

void WorkspaceConnection::read_events(short events, TimePoint now, DispatchResult& result) {
    std::array<char, 4096> buffer;
    std::size_t consumed = 0;
    unsigned attempts = 0;
    while (consumed < protocol::io_budget && attempts++ < 64) {
        const auto count = ::recv(events_.fd, buffer.data(), std::min(buffer.size(), protocol::io_budget - consumed), 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (events & (POLLERR | POLLHUP))
                    fail_connection(Error{ErrorCode::io, "events", "event stream failed"}, ConnectionPhase::disconnected, result);
                return;
            }
            fail_connection(protocol::io_error("events", errno), ConnectionPhase::disconnected, result);
            return;
        }
        if (count == 0) {
            fail_connection(Error{ErrorCode::io, "events", "event stream closed"}, ConnectionPhase::disconnected, result);
            return;
        }
        consumed += static_cast<std::size_t>(count);
        std::string_view bytes(buffer.data(), static_cast<std::size_t>(count));
        while (!bytes.empty()) {
            const auto newline = bytes.find('\n');
            auto line = bytes.substr(0, newline);
            if (line.size() > protocol::max_event_bytes - event_line_.size()) {
                fail_connection(Error{ErrorCode::limit_exceeded, "events", "event exceeds byte limit"}, ConnectionPhase::failed, result);
                return;
            }
            if (newline == bytes.npos || !event_line_.empty()) {
                event_line_.append(line);
                if (newline == bytes.npos) break;
                line = event_line_;
            }
            if (protocol::invalidates_workspaces(line))
                schedule_refresh(now + options_.event_coalesce);
            constexpr std::string_view prefix = "custom>>";
            if (!subscription_ready_ && line.starts_with(prefix) && line.substr(prefix.size()) == subscription_marker_)
                subscription_ready_ = true;
            event_line_.clear();
            bytes.remove_prefix(newline + 1);
        }
    }
}

void WorkspaceConnection::start_command(CommandKind kind, std::string request, TimePoint now, DispatchResult& result) {
    command_kind_ = kind;
    request_ = std::move(request);
    reply_.clear();
    command_deadline_ = now + options_.request_timeout;
    if (kind == CommandKind::switch_workspace)
        command_deadline_ = std::min(command_deadline_, pending_switch_->deadline);
    if (kind == CommandKind::subscription_marker) {
        command_deadline_ = std::min(command_deadline_, event_deadline_);
        marker_retry_at_ = now + std::chrono::milliseconds(20);
    }
    auto connected = protocol::connect_socket(command_.fd, target_.directory + "/.socket.sock");
    if (!connected) {
        fail_command(connected.error(), now, result);
        return;
    }
    command_phase_ = *connected ? CommandPhase::sending : CommandPhase::connecting;
    if (kind == CommandKind::snapshot) {
        refresh_at_.reset();
        snapshot_before_switch_ = false;
        status_.refresh_pending = true;
    }
}

void WorkspaceConnection::advance_command(short events, TimePoint now, DispatchResult& result) {
    const auto limit = command_kind_ == CommandKind::snapshot ? protocol::max_snapshot_bytes : protocol::max_switch_reply_bytes;
    const auto next = protocol::advance_request(command_.fd, events, command_phase_, request_, reply_, limit,
        command_kind_ == CommandKind::switch_workspace ? &pending_switch_->may_have_executed : nullptr);
    if (!next) { fail_command(next.error(), now, result); return; }
    command_phase_ = *next;
    if (command_phase_ == CommandPhase::complete) {
        if (command_kind_ == CommandKind::subscription_marker) {
            if (reply_ != "ok") {
                fail_command(Error{ErrorCode::protocol, "subscribe", "marker command failed: " + reply_}, now, result);
                return;
            }
        } else if (command_kind_ == CommandKind::snapshot) {
            auto candidate = protocol::parse_snapshot(reply_);
            if (!candidate) {
                fail_command(candidate.error(), now, result);
                return;
            }
            if (!snapshot_ || snapshot_->workspaces != candidate->workspaces || snapshot_->outputs != candidate->outputs ||
                snapshot_->focused_output != candidate->focused_output) {
                if (snapshot_ && snapshot_->revision == UINT64_MAX) {
                    fail_command(Error{ErrorCode::limit_exceeded, "snapshot", "snapshot revisions exhausted"}, now, result);
                    return;
                }
                candidate->revision = snapshot_ ? snapshot_->revision + 1 : 1;
                snapshot_ = std::make_shared<const Snapshot>(std::move(*candidate));
                result.snapshot_changed = true;
            }
            status_.phase = ConnectionPhase::ready;
            status_.refresh_pending = refresh_at_.has_value();
        } else {
            if (reply_ == "ok")
                complete_switch(SwitchOutcome::acknowledged, {}, result);
            else if (reply_.empty())
                complete_switch(SwitchOutcome::failed, Error{ErrorCode::protocol, "switch", "empty command reply"}, result);
            else
                complete_switch(SwitchOutcome::compositor_error, Error{ErrorCode::protocol, "switch", reply_}, result);
            snapshot_before_switch_ = true;
            schedule_refresh(now);
        }
        close_command();
        return;
    }
}

DispatchResult WorkspaceConnection::dispatch(const WaitSet& ready, TimePoint now) {
    if (ready.generation != wait_generation_ || ready.fds[0].fd != (running(status_.phase) ? events_.fd : -1) ||
        ready.fds[1].fd != (running(status_.phase) ? command_.fd : -1))
        return {};
    ++wait_generation_;
    const auto previous = status_;
    DispatchResult result;
    auto advance = [&] {
        if (!running(status_.phase))
            return;
        if ((ready.fds[0].revents | ready.fds[1].revents) & POLLNVAL)
            throw std::logic_error("host closed a borrowed connection descriptor");
        if (!subscription_ready_ && now >= event_deadline_) {
            fail_connection(Error{ErrorCode::timeout, "subscribe", "event subscription was not confirmed before its deadline"}, ConnectionPhase::failed, result);
            return;
        }
        if (event_connecting_) {
            if (ready.fds[0].revents) {
                if (auto connected = protocol::finish_connect(events_.fd); !connected) {
                    fail_connection(connected.error(), ConnectionPhase::failed, result);
                    return;
                }
                event_connecting_ = false;
                status_.phase = ConnectionPhase::synchronizing;
            }
        } else if (ready.fds[0].revents) {
            read_events(ready.fds[0].revents, now, result);
            if (!running(status_.phase))
                return;
        }

        if (pending_switch_ && now >= pending_switch_->deadline) {
            const Error error{ErrorCode::timeout, "switch", "switch deadline expired"};
            if (command_phase_ != CommandPhase::idle && command_kind_ == CommandKind::switch_workspace) {
                fail_command(error, now, result);
            } else {
                complete_switch(SwitchOutcome::failed, error, result);
            }
        }
        if (command_phase_ != CommandPhase::idle) {
            if (now >= command_deadline_)
                fail_command(Error{ErrorCode::timeout, "command", "command deadline expired"}, now, result);
            else if (ready.fds[1].revents)
                advance_command(ready.fds[1].revents, now, result);
        }
        if (!running(status_.phase) || event_connecting_ || command_phase_ != CommandPhase::idle)
            return;
        if (!subscription_ready_) {
            if (now >= marker_retry_at_)
                start_command(CommandKind::subscription_marker, "/dispatch hl.dsp.event(\"" + subscription_marker_ + "\")", now, result);
            return;
        }
        if (pending_switch_ && !snapshot_before_switch_) {
            const auto workspace = std::ranges::lower_bound(snapshot_->workspaces, pending_switch_->key, {}, &Workspace::key);
            if (workspace == snapshot_->workspaces.end() || workspace->key != pending_switch_->key) {
                complete_switch(SwitchOutcome::failed, Error{ErrorCode::unknown_workspace, "switch", "workspace disappeared before sending"}, result);
            } else {
                start_command(CommandKind::switch_workspace, std::move(pending_switch_->request), now, result);
                return;
            }
        }
        if (refresh_at_ && now >= *refresh_at_)
            start_command(CommandKind::snapshot, std::string(protocol::snapshot_request), now, result);
    };
    try {
        advance();
    } catch (...) {
        stop();
        throw;
    }
    result.status_changed = previous != status_;
    return result;
}

}
