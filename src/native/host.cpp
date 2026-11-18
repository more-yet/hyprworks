#include <hyprworks/host.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <linux/close_range.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hyprworks::host {
namespace {

void close_fd(int& fd) noexcept { if (fd >= 0) ::close(std::exchange(fd, -1)); }

void check(int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

void spawn_check(int result) {
    if (result) throw std::system_error(result, std::generic_category(), "spawn process");
}

void private_fd(int& fd) {
    if (fd >= 3) return;
    const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    check(replacement, "move private descriptor");
    close_fd(fd);
    fd = replacement;
}

std::vector<char*> argv_for(std::span<const std::string> arguments, const std::string& directory) {
    if (arguments.empty() || arguments.size() > 256 || arguments[0].empty() || directory.size() > 16384 || directory.contains('\0'))
        throw std::runtime_error("invalid process arguments or directory");
    std::size_t bytes = 0;
    std::vector<char*> argv;
    for (const auto& argument : arguments) {
        if (argument.contains('\0') || argument.size() > 65536 || (bytes += argument.size() + 1) > 1024 * 1024)
            throw std::runtime_error("process arguments exceed 64 KiB per argument or 1 MiB total");
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

void valid_path(const std::string& path) {
    if (path.empty() || path.size() > 16384 || path.contains('\0')) throw std::runtime_error("invalid file path");
}

}

std::expected<void, std::string> Timer::start(std::chrono::milliseconds delay, bool repeat) {
    const auto now = Clock::now();
    if (delay.count() < 0 || (repeat && delay.count() == 0) || delay > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now))
        return std::unexpected("invalid timer interval");
    interval_ = repeat ? std::chrono::duration_cast<Clock::duration>(delay) : Clock::duration::zero();
    deadline_ = now + delay;
    return {};
}

void Timer::stop() noexcept { deadline_.reset(); }
std::optional<Clock::time_point> Timer::deadline() const noexcept { return deadline_; }

std::uint64_t Timer::dispatch(Clock::time_point now) noexcept {
    if (!deadline_ || now < *deadline_) return 0;
    if (interval_ == Clock::duration::zero()) { deadline_.reset(); return 1; }
    const auto count = (now - *deadline_) / interval_ + 1;
    const auto remainder = (now - *deadline_) % interval_;
    const auto next = interval_ - remainder;
    if (next > Clock::time_point::max() - now) deadline_.reset();
    else deadline_ = now + next;
    return static_cast<std::uint64_t>(count);
}

struct Process::State {
    pid_t pid = -1;
    int pidfd = -1, output = -1, error = -1;
    std::uint64_t generation = 0;
    std::size_t limit = 0;
    Clock::time_point deadline{};
    ProcessResult pending;
    std::optional<ProcessResult> result;
    bool exited = false;

    void finish(ProcessOutcome outcome) noexcept {
        if (pid < 0) return;
        // Keep the leader unreaped until group cleanup so its pid cannot be reused.
        ::kill(-pid, SIGKILL);
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited == pid) {
            if (WIFEXITED(status)) pending.exit_code = WEXITSTATUS(status);
            if (WIFSIGNALED(status)) pending.signal = WTERMSIG(status);
        }
        pending.outcome = outcome;
        if (outcome == ProcessOutcome::exited && pending.signal) pending.outcome = ProcessOutcome::signalled;
        result = std::move(pending);
        pid = -1;
        close_fd(pidfd); close_fd(output); close_fd(error);
        ++generation;
    }
};

Process::Process() : state_(std::make_unique<State>()) {}
Process::~Process() { cancel(); }

std::expected<void, std::string> Process::start(std::span<const std::string> arguments, const ProcessOptions& options) {
    auto& state = *state_;
    if (state.pid >= 0) return std::unexpected("process is already running");
    int out[2]{-1, -1}, err[2]{-1, -1};
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool have_actions = false, have_attributes = false;
    try {
        const auto argv = argv_for(arguments, options.directory);
        if (options.timeout.count() <= 0 || options.timeout > std::chrono::hours(24) || options.output_limit > 16 * 1024 * 1024)
            throw std::runtime_error("process timeout must be 1 ms..24 h and output limit at most 16 MiB per stream");
        check(pipe2(out, O_CLOEXEC), "stdout pipe"); check(pipe2(err, O_CLOEXEC), "stderr pipe");
        for (auto* fd : {&out[0], &out[1], &err[0], &err[1]}) private_fd(*fd);
        check(fcntl(out[0], F_SETFL, O_NONBLOCK), "nonblocking stdout"); check(fcntl(err[0], F_SETFL, O_NONBLOCK), "nonblocking stderr");
        spawn_check(posix_spawn_file_actions_init(&actions)); have_actions = true;
        spawn_check(posix_spawnattr_init(&attributes)); have_attributes = true;
        spawn_check(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        spawn_check(posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO));
        spawn_check(posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO));
        if (!options.directory.empty()) spawn_check(posix_spawn_file_actions_addchdir_np(&actions, options.directory.c_str()));
        spawn_check(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
        sigset_t empty, defaults;
        sigemptyset(&empty); sigfillset(&defaults);
        spawn_check(posix_spawnattr_setsigmask(&attributes, &empty));
        spawn_check(posix_spawnattr_setsigdefault(&attributes, &defaults));
        spawn_check(posix_spawnattr_setpgroup(&attributes, 0));
        spawn_check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
        state.pending = {}; state.result.reset(); state.exited = false;
        pid_t child = -1;
        spawn_check(posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ));
        state.pid = child;
        state.output = std::exchange(out[0], -1); state.error = std::exchange(err[0], -1);
        state.pidfd = static_cast<int>(syscall(SYS_pidfd_open, state.pid, 0));
        check(state.pidfd, "process pidfd");
        state.limit = options.output_limit; state.deadline = Clock::now() + options.timeout;
        ++state.generation;
        posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
        close_fd(out[1]); close_fd(err[1]);
        return {};
    } catch (const std::exception& error) {
        if (have_actions) posix_spawn_file_actions_destroy(&actions);
        if (have_attributes) posix_spawnattr_destroy(&attributes);
        close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
        if (state.pid >= 0) state.finish(ProcessOutcome::failed);
        return std::unexpected(error.what());
    }
}

void Process::cancel() noexcept { state_->finish(ProcessOutcome::cancelled); }
bool Process::running() const noexcept { return state_->pid >= 0; }
const std::optional<ProcessResult>& Process::result() const noexcept { return state_->result; }

ProcessWait Process::prepare_wait() {
    auto& state = *state_;
    ProcessWait wait;
    wait.generation = ++state.generation;
    if (state.pid < 0) return wait;
    wait.fds = {{{state.output, POLLIN, 0}, {state.error, POLLIN, 0}, {state.exited ? -1 : state.pidfd, POLLIN, 0}}};
    wait.deadline = state.deadline;
    return wait;
}

bool Process::dispatch(const ProcessWait& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fds[0].fd != state.output || ready.fds[1].fd != state.error ||
        ready.fds[2].fd != (state.exited ? -1 : state.pidfd)) throw std::logic_error("dispatch requires the current process wait set");
    ++state.generation;
    if (state.pid < 0) return false;
    try {
        for (unsigned stream = 0; stream < 2; ++stream) {
            int& fd = stream == 0 ? state.output : state.error;
            auto& text = stream == 0 ? state.pending.stdout_text : state.pending.stderr_text;
            if (fd < 0 || !ready.fds[stream].revents) continue;
            for (unsigned reads = 0; reads < 4; ++reads) {
                char buffer[16384];
                const auto size = ::read(fd, buffer, sizeof(buffer));
                if (size < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    check(-1, "read process output");
                }
                if (!size) { close_fd(fd); break; }
                const auto count = static_cast<std::size_t>(size);
                const auto accepted = std::min(count, state.limit - text.size());
                text.append(buffer, accepted);
                if (accepted != count) { state.pending.truncated = true; throw std::runtime_error("process output limit exceeded"); }
            }
        }
        if (!state.exited && ready.fds[2].revents) {
            siginfo_t info{};
            check(waitid(P_PIDFD, static_cast<id_t>(state.pidfd), &info, WEXITED | WNOHANG | WNOWAIT), "observe process exit");
            state.exited = info.si_pid != 0;
        }
        if (state.exited && state.output < 0 && state.error < 0) { state.finish(ProcessOutcome::exited); return true; }
        if (Clock::now() >= state.deadline) { state.finish(ProcessOutcome::timed_out); return true; }
        return false;
    } catch (const std::exception& error) {
        state.pending.error = error.what(); state.finish(ProcessOutcome::failed); return true;
    }
}

std::expected<void, std::string> launch(std::span<const std::string> arguments, const std::string& directory) {
    int report[2]{-1, -1}, nullfd = -1;
    pid_t child = -1;
    try {
        const auto argv = argv_for(arguments, directory);
        std::string executable = arguments[0];
        if (!executable.contains('/')) {
            const auto base = directory.empty() ? std::filesystem::current_path() : std::filesystem::absolute(directory);
            const char* env_path = std::getenv("PATH");
            const std::string search = env_path ? env_path : "/usr/bin:/bin";
            bool found = false;
            for (std::size_t begin = 0; begin <= search.size();) {
                const auto end = search.find(':', begin);
                const auto part = search.substr(begin, end == std::string::npos ? end : end - begin);
                auto candidate = (base / part / executable).string();
                struct stat status{};
                if (stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode) && access(candidate.c_str(), X_OK) == 0) {
                    executable = std::move(candidate); found = true; break;
                }
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (!found) throw std::runtime_error("application executable was not found in PATH");
        }
        nullfd = open("/dev/null", O_RDWR | O_CLOEXEC); check(nullfd, "open /dev/null");
        check(pipe2(report, O_CLOEXEC), "launch result pipe");
        for (auto* fd : {&nullfd, &report[0], &report[1]}) private_fd(*fd);
        child = _Fork(); check(child, "fork launch");
        if (!child) {
            ::close(report[0]);
            int error = 0;
            if (setsid() < 0) error = errno;
            if (!error) {
                const pid_t detached = _Fork();
                if (detached < 0) error = errno;
                else if (detached > 0) _exit(0);
            }
            if (!error && !directory.empty() && chdir(directory.c_str()) < 0) error = errno;
            for (int fd = 0; !error && fd < 3; ++fd) if (dup2(nullfd, fd) < 0) error = errno;
            if (!error && syscall(SYS_close_range, 3U, UINT_MAX, CLOSE_RANGE_CLOEXEC) < 0) error = errno;
            sigset_t empty;
            sigemptyset(&empty);
            if (!error && sigprocmask(SIG_SETMASK, &empty, nullptr) < 0) error = errno;
            if (!error) { execve(executable.c_str(), argv.data(), environ); error = errno; }
            while (::write(report[1], &error, sizeof(error)) < 0 && errno == EINTR) {}
            _exit(127);
        }
        close_fd(report[1]); close_fd(nullfd);
        int status;
        while (waitpid(child, &status, 0) < 0) if (errno != EINTR) { child = -1; check(-1, "reap launch helper"); }
        child = -1;
        int error = 0;
        ssize_t received;
        do { received = ::read(report[0], &error, sizeof(error)); } while (received < 0 && errno == EINTR);
        check(static_cast<int>(received), "read launch result");
        close_fd(report[0]);
        if (received) throw std::system_error(error, std::generic_category(), "launch application");
        return {};
    } catch (const std::exception& error) {
        close_fd(report[0]); close_fd(report[1]); close_fd(nullfd);
        return std::unexpected(error.what());
    }
}

std::expected<std::string, std::string> read_file(const std::string& path, std::size_t limit) {
    int fd = -1;
    try {
        valid_path(path);
        if (limit > 16 * 1024 * 1024) throw std::runtime_error("file read limit exceeds 16 MiB");
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK); check(fd, "open file");
        struct stat status{}; check(fstat(fd, &status), "stat file");
        if (!S_ISREG(status.st_mode)) throw std::runtime_error("file is not regular");
        std::string result;
        for (;;) {
            char buffer[16384];
            const auto size = ::read(fd, buffer, sizeof(buffer));
            if (size < 0 && errno == EINTR) continue;
            check(static_cast<int>(size), "read file");
            if (!size) break;
            if (static_cast<std::size_t>(size) > limit - result.size()) throw std::runtime_error("file read limit exceeded");
            result.append(buffer, static_cast<std::size_t>(size));
        }
        close_fd(fd); return result;
    } catch (const std::exception& error) { close_fd(fd); return std::unexpected(error.what()); }
}

std::expected<void, std::string> write_file(const std::string& path, const std::string& contents) {
    int fd = -1, parent = -1;
    std::string temporary;
    bool created = false, renamed = false;
    try {
        valid_path(path);
        if (contents.size() > 16 * 1024 * 1024) throw std::runtime_error("file write exceeds 16 MiB");
        const auto destination = std::filesystem::absolute(path);
        parent = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); check(parent, "open destination directory");
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            std::uint64_t random;
            const auto received = getrandom(&random, sizeof(random), 0);
            if (received < 0 && errno == EINTR) continue;
            check(static_cast<int>(received), "temporary filename randomness");
            if (received != sizeof(random)) throw std::runtime_error("incomplete temporary filename randomness");
            temporary = ".hyprworks-" + std::to_string(random);
            fd = openat(parent, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) { created = true; break; }
            if (errno != EEXIST) check(-1, "create temporary file");
        }
        if (!created) throw std::runtime_error("temporary filename attempts exhausted");
        std::size_t written = 0;
        while (written < contents.size()) {
            const auto count = ::write(fd, contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR) continue;
            check(static_cast<int>(count), "write file");
            if (!count) throw std::runtime_error("file write made no progress");
            written += static_cast<std::size_t>(count);
        }
        check(fsync(fd), "sync file");
        close_fd(fd);
        check(renameat(parent, temporary.c_str(), parent, destination.filename().c_str()), "replace file"); renamed = true;
        check(fsync(parent), "sync destination directory");
        close_fd(parent); return {};
    } catch (const std::exception& error) {
        close_fd(fd);
        if (created && !renamed) unlinkat(parent, temporary.c_str(), 0);
        close_fd(parent);
        return std::unexpected(std::string(renamed ? "file replaced, durability uncertain: " : "") + error.what());
    }
}

FileWatch::~FileWatch() { stop(); }
void FileWatch::stop() noexcept { close_fd(fd_); filename_.clear(); }
bool FileWatch::running() const noexcept { return fd_ >= 0; }
pollfd FileWatch::prepare_wait() const noexcept { return {fd_, POLLIN, 0}; }

std::expected<void, std::string> FileWatch::start(const std::string& path, bool directory) {
    stop();
    try {
        valid_path(path);
        const auto target = std::filesystem::absolute(path).lexically_normal();
        filename_ = directory ? "" : target.filename().string();
        fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK); check(fd_, "create file watch");
        check(inotify_add_watch(fd_, (directory ? target : target.parent_path()).c_str(),
            IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR), "watch directory");
        return {};
    } catch (const std::exception& error) { stop(); return std::unexpected(error.what()); }
}

FileUpdate FileWatch::dispatch() {
    FileUpdate update;
    if (fd_ < 0) return update;
    for (unsigned count = 0; count < 4; ++count) {
        alignas(inotify_event) char buffer[16384];
        const auto size = ::read(fd_, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        check(static_cast<int>(size), "read file watch");
        if (!size) break;
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(size);) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
            if (event->mask & IN_Q_OVERFLOW) update.overflow = update.changed = true;
            if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) update.invalidated = update.changed = true;
            if (event->len && (filename_.empty() || filename_ == event->name)) update.changed = true;
            offset += sizeof(inotify_event) + event->len;
        }
    }
    if (update.invalidated) stop();
    return update;
}

}
