#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::hyprland {

enum class WorkspaceType { normal, special };

struct WorkspaceKey {
    WorkspaceType type = WorkspaceType::normal;
    std::string address;
    auto operator<=>(const WorkspaceKey&) const = default;
};

struct Workspace {
    WorkspaceKey key;
    std::string name;
    std::optional<std::uint32_t> number;
    std::optional<std::int64_t> output_id;
    bool operator==(const Workspace&) const = default;
};

struct Output {
    std::int64_t id = 0;
    std::string name;
    bool enabled = false;
    std::optional<WorkspaceKey> active_workspace;
    std::optional<WorkspaceKey> special_workspace;
    bool operator==(const Output&) const = default;
};

struct Snapshot {
    std::uint64_t revision = 0;
    std::vector<Workspace> workspaces;
    std::vector<Output> outputs;
    std::optional<std::int64_t> focused_output;
};

enum class ErrorCode {
    invalid_target, unavailable, io, timeout, protocol, limit_exceeded,
    not_ready, unknown_workspace, busy
};

struct Error {
    ErrorCode code;
    std::string operation;
    std::string message;
    int system_error = 0;
    bool operator==(const Error&) const = default;
};

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct SessionTarget {
    std::string directory;
};

std::expected<SessionTarget, Error> session_from_environment();

enum class ConnectionPhase { stopped, connecting, synchronizing, ready, disconnected, failed };

struct ConnectionStatus {
    ConnectionPhase phase = ConnectionPhase::stopped;
    bool refresh_pending = false;
    std::optional<Error> error;
    bool operator==(const ConnectionStatus&) const = default;
};

struct ConnectionOptions {
    std::chrono::milliseconds request_timeout{2000};
    std::chrono::milliseconds event_coalesce{16};
};

struct WaitSet {
    std::array<pollfd, 2> fds{{{-1, 0, 0}, {-1, 0, 0}}};
    std::optional<TimePoint> deadline;

private:
    friend class WorkspaceConnection;
    std::uint64_t generation = 0;
};

enum class SwitchOutcome { acknowledged, compositor_error, failed, cancelled };

struct SwitchResult {
    std::uint64_t id;
    SwitchOutcome outcome;
    bool may_have_executed;
    std::optional<Error> error;
};

struct DispatchResult {
    bool snapshot_changed = false;
    bool status_changed = false;
    std::optional<SwitchResult> switch_result;
};

enum class SocketPhase { idle, connecting, sending, reading, complete };

class WorkspaceConnection {
public:
    explicit WorkspaceConnection(ConnectionOptions options = {});
    ~WorkspaceConnection();
    WorkspaceConnection(const WorkspaceConnection&) = delete;
    WorkspaceConnection& operator=(const WorkspaceConnection&) = delete;

    std::expected<void, Error> start(SessionTarget target, TimePoint now = Clock::now());
    std::optional<SwitchResult> stop();
    WaitSet prepare_wait() const;
    DispatchResult dispatch(const WaitSet& ready, TimePoint now = Clock::now());
    std::shared_ptr<const Snapshot> snapshot() const noexcept { return snapshot_; }
    const ConnectionStatus& status() const noexcept { return status_; }
    std::expected<std::uint64_t, Error> switch_workspace(const WorkspaceKey& key, TimePoint now = Clock::now());
    std::expected<void, Error> refresh(TimePoint now = Clock::now());

private:
    struct Socket {
        int fd = -1;
        Socket() = default;
        ~Socket();
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        void reset() noexcept;
    };
    enum class CommandKind { subscription_marker, snapshot, switch_workspace };
    using CommandPhase = SocketPhase;
    struct PendingSwitch {
        std::uint64_t id;
        WorkspaceKey key;
        std::string request;
        TimePoint deadline;
        bool may_have_executed = false;
    };

    void schedule_refresh(TimePoint when);
    void complete_switch(SwitchOutcome outcome, std::optional<Error> error, DispatchResult& result);
    void fail_connection(Error error, ConnectionPhase phase, DispatchResult& result);
    void fail_command(Error error, TimePoint now, DispatchResult& result);
    void close_command() noexcept;
    void read_events(short events, TimePoint now, DispatchResult& result);
    void advance_command(short events, TimePoint now, DispatchResult& result);
    void start_command(CommandKind kind, std::string request, TimePoint now, DispatchResult& result);

    ConnectionOptions options_;
    SessionTarget target_;
    ConnectionStatus status_;
    std::shared_ptr<const Snapshot> snapshot_;
    Socket events_;
    Socket command_;
    bool event_connecting_ = false;
    bool subscription_ready_ = false;
    TimePoint event_deadline_{};
    TimePoint marker_retry_at_{};
    std::string subscription_marker_;
    std::string event_line_;
    CommandKind command_kind_ = CommandKind::snapshot;
    CommandPhase command_phase_ = CommandPhase::idle;
    TimePoint command_deadline_{};
    std::string request_;
    std::string reply_;
    std::optional<PendingSwitch> pending_switch_;
    std::optional<TimePoint> refresh_at_;
    bool snapshot_before_switch_ = true;
    std::uint64_t last_request_id_ = 0;
    std::uint64_t wait_generation_ = 0;
};

}
