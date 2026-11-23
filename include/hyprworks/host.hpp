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
    /// Coalesces missed deadlines into an elapsed-interval count.
    std::uint64_t dispatch(Clock::time_point now = Clock::now()) noexcept;
private:
    std::optional<Clock::time_point> deadline_;
    Clock::duration interval_{};
};

struct ProcessOptions {
    std::string directory;
    std::chrono::milliseconds timeout{};
    std::size_t output_limit = 0;
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

/// Exclusively owns child reaping and process-group cancellation. The host
/// supplies readiness; the result remains borrowed until the next start.
class Process {
public:
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    std::expected<void, std::string> start(std::span<const std::string> arguments, const ProcessOptions& options);
    void cancel() noexcept;
    bool running() const noexcept;
    const std::optional<ProcessResult>& result() const noexcept;
    ProcessWait prepare_wait();
    bool dispatch(const ProcessWait& ready);
private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Transfers application lifetime to a detached process; returns after exec.
std::expected<void, std::string> launch(std::span<const std::string> arguments, const std::string& directory = {});
std::expected<std::string, std::string> read_file(const std::string& path, std::size_t limit);
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
    /// File watches follow the parent directory across atomic replacement.
    /// An invalidated watch needs its targets registered again on existing directories.
    std::expected<void, std::string> start(const std::string& path, bool directory = false);
    std::expected<void, std::string> add(const std::string& path, bool directory = false);
    void stop() noexcept;
    pollfd prepare_wait() const noexcept;
    FileUpdate dispatch();
    bool running() const noexcept;
private:
    int fd_ = -1;
    struct Target {
        int descriptor = -1;
        std::string path;
        std::size_t name_offset;
    };
    std::vector<Target> targets_;
};

}
