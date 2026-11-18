#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <poll.h>

namespace hyprworks::host {

using Clock = std::chrono::steady_clock;

class Timer {
public:
    std::expected<void, std::string> start(std::chrono::milliseconds delay, bool repeat = false);
    void stop() noexcept;
    std::optional<Clock::time_point> deadline() const noexcept;
    // A delayed dispatch produces one notification carrying the number of elapsed intervals.
    std::uint64_t dispatch(Clock::time_point now = Clock::now()) noexcept;
private:
    std::optional<Clock::time_point> deadline_;
    Clock::duration interval_{};
};

struct ProcessOptions {
    std::string directory;
    std::chrono::milliseconds timeout{30000};
    std::size_t output_limit = 1024 * 1024;
};

enum class ProcessOutcome { exited, signalled, cancelled, timed_out, failed };
struct ProcessResult {
    ProcessOutcome outcome = ProcessOutcome::exited;
    int exit_code = 0, signal = 0;
    std::string stdout_text, stderr_text, error;
    bool truncated = false;
};

struct ProcessWait {
    std::array<pollfd, 3> fds{{{-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}};
    std::optional<Clock::time_point> deadline;
private:
    friend class Process;
    std::uint64_t generation = 0;
};

// Owns a process group, its output pipes and child reaping. Hosts must not reap this child separately.
class Process {
public:
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    std::expected<void, std::string> start(std::span<const std::string> arguments, const ProcessOptions& options = {});
    void cancel() noexcept;
    bool running() const noexcept;
    const std::optional<ProcessResult>& result() const noexcept;
    ProcessWait prepare_wait();
    bool dispatch(const ProcessWait& ready);
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Detached launch: no captured output, no invocation-owned cancellation, no zombie child in the host.
std::expected<void, std::string> launch(std::span<const std::string> arguments, const std::string& directory = {});
std::expected<std::string, std::string> read_file(const std::string& path, std::size_t limit = 1024 * 1024);
std::expected<void, std::string> write_file(const std::string& path, const std::string& contents);

struct FileUpdate {
    bool changed = false, overflow = false, invalidated = false;
};

class FileWatch {
public:
    FileWatch() = default;
    ~FileWatch();
    FileWatch(const FileWatch&) = delete;
    FileWatch& operator=(const FileWatch&) = delete;
    // Watches a file through its parent directory, or the immediate entries of a directory.
    std::expected<void, std::string> start(const std::string& path, bool directory = false);
    void stop() noexcept;
    pollfd prepare_wait() const noexcept;
    FileUpdate dispatch();
    bool running() const noexcept;
private:
    int fd_ = -1;
    std::string filename_;
};

}
