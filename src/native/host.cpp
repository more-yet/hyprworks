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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pthread.h>
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

std::string executable_path(const std::string& program, const std::string& directory, const char* path) {
    if (program.contains('/')) return program;
    const auto base = std::filesystem::absolute(directory.empty() ? "." : directory);
    std::string_view search(path ? path : "/bin:/usr/bin");
    for (;;) {
        const auto colon = search.find(':');
        const auto candidate = base / search.substr(0, colon) / program;
        struct stat status{};
        if (stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode) && access(candidate.c_str(), X_OK) == 0)
            return candidate.string();
        if (colon == search.npos) throw std::runtime_error("executable not found in PATH: " + program);
        search.remove_prefix(colon + 1);
    }
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
    int pidfd = -1, output = -1, error = -1, input = -1;
    std::uint64_t generation = 0;
    std::size_t limit = 0;
    Clock::time_point deadline{};
    ProcessResult pending;
    std::optional<ProcessResult> result;
    bool exited = false;
    bool terminal = false, eof = true;
    std::size_t input_limit = 0, sent = 0;
    std::string queue;

    void end_input() noexcept {
        pending.stdin_discarded += queue.size() - sent;
        close_fd(input); queue.clear(); sent = 0; eof = true;
    }

    void finish(ProcessOutcome outcome) noexcept {
        if (pid < 0) return;
        // The unreaped leader reserves the process-group ID through cancellation.
        ::kill(-pid, SIGKILL);
        if (!exited) ::kill(pid, SIGKILL);
        end_input();
        close_fd(output); close_fd(error);
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
        close_fd(pidfd);
        ++generation;
    }
};

Process::Process() : state_(std::make_unique<State>()) {}
Process::~Process() { cancel(); }

std::expected<void, std::string> Process::start(std::span<const std::string> arguments, const ProcessOptions& options) {
    auto& state = *state_;
    if (state.pid >= 0) return std::unexpected("process is already running");
    int in[2]{-1, -1}, out[2]{-1, -1}, err[2]{-1, -1};
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool have_actions = false, have_attributes = false;
    try {
        const auto argv = argv_for(arguments, options.directory);
        if (options.timeout.count() <= 0 || options.timeout > std::chrono::hours(24) || options.output_limit > 16 * 1024 * 1024 ||
            !options.input_limit || options.input_limit > 1024 * 1024)
            throw std::runtime_error("invalid process timeout or stream limit");
        std::vector<std::string> overrides;
        std::vector<char*> environment;
        const char* search_path = getenv("PATH");
        if (!options.environment.empty()) {
            if (options.environment.size() > 128) throw std::runtime_error("environment exceeds 128 overrides");
            std::size_t size = 0;
            for (const auto& [name, value] : options.environment) {
                if (name.empty() || name.size() > 255 || name.contains('=') || name.contains('\0') ||
                    value.size() > 65536 || value.contains('\0') || (size += name.size() + value.size() + 2) > 1024 * 1024)
                    throw std::runtime_error("invalid process environment");
                if (name == "PATH") search_path = value.c_str();
                if (std::ranges::any_of(overrides, [&](const std::string& entry) {
                    return std::string_view(entry).substr(0, entry.find('=')) == name;
                })) throw std::runtime_error("duplicate process environment override");
                overrides.push_back(name + "=" + value);
            }
            for (char** variable = environ; *variable; ++variable) {
                const std::string_view entry(*variable);
                if (!std::ranges::any_of(options.environment, [&](const auto& pair) { return entry.substr(0, entry.find('=')) == pair.first; }))
                    environment.push_back(*variable);
            }
            for (auto& entry : overrides) environment.push_back(entry.data());
            environment.push_back(nullptr);
        }
        const auto executable = executable_path(arguments[0], options.directory, search_path);
        spawn_check(posix_spawn_file_actions_init(&actions)); have_actions = true;
        spawn_check(posix_spawnattr_init(&attributes)); have_attributes = true;
        if (!options.directory.empty()) spawn_check(posix_spawn_file_actions_addchdir_np(&actions, options.directory.c_str()));
        if (options.terminal) {
            if (!options.terminal->rows || !options.terminal->columns) throw std::runtime_error("terminal dimensions must be positive");
            out[0] = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK); check(out[0], "open PTY master");
            private_fd(out[0]);
            check(grantpt(out[0]), "grant PTY"); check(unlockpt(out[0]), "unlock PTY");
            char slave[128]; spawn_check(ptsname_r(out[0], slave, sizeof(slave)));
            termios settings{};
            check(tcgetattr(out[0], &settings), "read terminal settings");
            if (options.terminal->raw) cfmakeraw(&settings);
            if (options.terminal->echo) settings.c_lflag |= ECHO; else settings.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            check(tcsetattr(out[0], TCSANOW, &settings), "set terminal settings");
            const winsize size{options.terminal->rows, options.terminal->columns, 0, 0};
            check(ioctl(out[0], TIOCSWINSZ, &size), "set terminal size");
            // POSIX_SPAWN_SETSID runs before file actions. Opening the slave makes it the controlling terminal.
            spawn_check(posix_spawn_file_actions_addopen(&actions, 0, slave, O_RDWR, 0));
            spawn_check(posix_spawn_file_actions_adddup2(&actions, 0, 1));
            spawn_check(posix_spawn_file_actions_adddup2(&actions, 0, 2));
        } else {
            const auto stream = [&](int (&pipe)[2], const std::string& path, int target) {
                if (!path.empty()) {
                    valid_path(path);
                    spawn_check(posix_spawn_file_actions_addopen(&actions, target, path.c_str(),
                        target ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW, 0600));
                    return;
                }
                check(pipe2(pipe, O_CLOEXEC), "create process pipe");
                private_fd(pipe[0]); private_fd(pipe[1]);
                const auto child_end = target == STDIN_FILENO ? 0 : 1;
                check(fcntl(pipe[1 - child_end], F_SETFL, O_NONBLOCK), "set process pipe nonblocking");
                spawn_check(posix_spawn_file_actions_adddup2(&actions, pipe[child_end], target));
            };
            stream(in, options.stdin_file, 0); stream(out, options.stdout_file, 1); stream(err, options.stderr_file, 2);
        }
        spawn_check(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
        sigset_t empty, defaults;
        sigemptyset(&empty); sigfillset(&defaults);
        spawn_check(posix_spawnattr_setsigmask(&attributes, &empty));
        spawn_check(posix_spawnattr_setsigdefault(&attributes, &defaults));
        spawn_check(posix_spawnattr_setpgroup(&attributes, 0));
        spawn_check(posix_spawnattr_setflags(&attributes, (options.terminal ? POSIX_SPAWN_SETSID : POSIX_SPAWN_SETPGROUP) | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
        pid_t child = -1;
        char* const* env = environment.empty() ? environ : environment.data();
        spawn_check(posix_spawn(&child, executable.c_str(), &actions, &attributes, argv.data(), env));
        const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, child, 0));
        if (pidfd < 0) {
            const int error = errno;
            ::kill(-child, SIGKILL); ::kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            throw std::system_error(error, std::generic_category(), "process pidfd");
        }
        state.pending = {}; state.result.reset(); state.exited = false;
        state.terminal = options.terminal.has_value();
        state.eof = !state.terminal && in[1] < 0;
        state.input_limit = options.input_limit;
        state.pid = child;
        state.input = std::exchange(in[1], -1);
        state.output = std::exchange(out[0], -1); state.error = std::exchange(err[0], -1);
        state.pidfd = pidfd;
        state.limit = options.output_limit; state.deadline = Clock::now() + options.timeout;
        ++state.generation;
        posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
        close_fd(in[0]); close_fd(out[1]); close_fd(err[1]);
        return {};
    } catch (const std::exception& error) {
        if (have_actions) posix_spawn_file_actions_destroy(&actions);
        if (have_attributes) posix_spawnattr_destroy(&attributes);
        close_fd(in[0]); close_fd(in[1]); close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
        return std::unexpected(error.what());
    }
}

void Process::cancel() noexcept { state_->finish(ProcessOutcome::cancelled); }
bool Process::running() const noexcept { return state_->pid >= 0; }
const std::optional<ProcessResult>& Process::result() const noexcept { return state_->result; }

std::string_view Process::output(bool stderr_stream) const noexcept {
    const auto& r = state_->result ? *state_->result : state_->pending;
    return stderr_stream ? r.stderr_text : r.stdout_text;
}

std::size_t Process::queued() const noexcept { return state_->queue.size() - state_->sent; }
std::size_t Process::input_discarded() const noexcept {
    return state_->result ? state_->result->stdin_discarded : state_->pending.stdin_discarded;
}

std::expected<std::size_t, std::string> Process::write(std::string_view data) {
    auto& s = *state_;
    if (s.eof) return std::unexpected("process input is closed");
    const auto accepted = std::min(data.size(), s.input_limit - queued());
    if (!accepted) return 0;
    if (accepted > s.input_limit - s.queue.size()) { s.queue.erase(0, s.sent); s.sent = 0; }
    s.queue.append(data.substr(0, accepted));
    ++s.generation;
    return accepted;
}

std::expected<void, std::string> Process::close_input() {
    auto& s = *state_;
    if (s.terminal) return std::unexpected("a PTY is bidirectional; send the terminal EOF character or a signal");
    s.eof = true;
    if (!queued()) s.end_input();
    ++s.generation;
    return {};
}

std::expected<void, std::string> Process::resize(unsigned short rows, unsigned short columns) {
    if (!state_->terminal || state_->output < 0 || !rows || !columns) return std::unexpected("resize requires an active PTY and positive dimensions");
    const winsize size{rows, columns, 0, 0};
    if (ioctl(state_->output, TIOCSWINSZ, &size) < 0) return std::unexpected(std::strerror(errno));
    return {};
}

std::expected<void, std::string> Process::hangup() {
    if (!state_->terminal || state_->output < 0) return std::unexpected("hangup requires an active PTY");
    state_->end_input();
    close_fd(state_->output);
    ++state_->generation;
    return {};
}

std::expected<void, std::string> Process::signal(int number) {
    if (!running() || number <= 0 || number >= NSIG) return std::unexpected("signal requires an active process and valid signal");
    const auto group = state_->terminal && state_->output >= 0 ? tcgetpgrp(state_->output) : state_->pid;
    if (!group) return std::unexpected("terminal has no foreground process group");
    if (group < 0 || kill(-group, number) < 0) return std::unexpected(std::strerror(errno));
    return {};
}

ProcessWait Process::prepare_wait() {
    auto& state = *state_;
    ProcessWait wait;
    wait.generation = ++state.generation;
    if (state.pid < 0) return wait;
    wait.fds = {{{state.output, POLLIN, 0}, {state.error, POLLIN, 0}, {state.exited ? -1 : state.pidfd, POLLIN, 0},
        {queued() ? state.input : -1, POLLOUT, 0}}};
    if (state.terminal && queued()) wait.fds[0].events |= POLLOUT;
    wait.deadline = state.deadline;
    return wait;
}

bool Process::dispatch(const ProcessWait& ready) {
    auto& state = *state_;
    if (ready.generation != state.generation || ready.fds[0].fd != state.output || ready.fds[1].fd != state.error ||
        ready.fds[2].fd != (state.exited ? -1 : state.pidfd) || ready.fds[3].fd != (queued() ? state.input : -1)) return false;
    ++state.generation;
    if (state.pid < 0) return false;
    try {
        const auto& writer = ready.fds[state.terminal ? 0 : 3];
        if (queued() && (writer.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            const auto count = write_pipe(writer.fd, state.queue.data() + state.sent, std::min<std::size_t>(queued(), 65536));
            if (count < 0 && (errno == EPIPE || (state.terminal && errno == EIO))) state.end_input();
            else if (count < 0 && errno != EAGAIN && errno != EINTR) check(-1, "write process input");
            else if (count > 0) {
                state.sent += static_cast<std::size_t>(count);
                if (!queued()) { state.queue.clear(); state.sent = 0; if (state.eof) state.end_input(); }
            }
        }
        for (unsigned stream = 0; stream < 2; ++stream) {
            int& fd = stream == 0 ? state.output : state.error;
            auto& text = stream == 0 ? state.pending.stdout_text : state.pending.stderr_text;
            if (!(ready.fds[stream].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) || fd < 0) continue;
            for (unsigned reads = 0; reads < 4; ++reads) {
                char buffer[16384];
                const auto size = ::read(fd, buffer, sizeof(buffer));
                if (size == 0 || (size < 0 && state.terminal && errno == EIO)) {
                    close_fd(fd);
                    if (state.terminal) state.end_input();
                    break;
                }
                if (size < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    check(-1, "read process output");
                }
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
    try {
        const auto argv = argv_for(arguments, directory);
        const auto executable = executable_path(arguments[0], directory, getenv("PATH"));
        nullfd = open("/dev/null", O_RDWR | O_CLOEXEC); check(nullfd, "open /dev/null");
        check(pipe2(report, O_CLOEXEC), "launch result pipe");
        for (auto* fd : {&nullfd, &report[0], &report[1]}) private_fd(*fd);
        const pid_t child = _Fork(); check(child, "fork launch");
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
        while (waitpid(child, nullptr, 0) < 0) if (errno != EINTR) check(-1, "reap launch helper");
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
        if (status.st_size > 0) result.reserve(std::min(static_cast<std::uintmax_t>(status.st_size), static_cast<std::uintmax_t>(limit)));
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

ssize_t write_pipe(int fd, const char* data, std::size_t size) {
    sigset_t block, previous, pending;
    sigemptyset(&block); sigaddset(&block, SIGPIPE);
    const int error = pthread_sigmask(SIG_BLOCK, &block, &previous);
    if (error) { errno = error; return -1; }
    sigpending(&pending); const bool existed = sigismember(&pending, SIGPIPE);
    const auto result = write(fd, data, size); const int saved = errno;
    if (result < 0 && saved == EPIPE && !existed) {
        timespec timeout{};
        while (sigtimedwait(&block, nullptr, &timeout) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr); errno = saved; return result;
}

FileWriter::FileWriter(const std::string& path) {
    valid_path(path);
    const auto target = std::filesystem::absolute(path);
    destination_ = target.filename().string();
    parent_ = open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(parent_, "open destination directory");
    try {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            std::uint64_t random;
            const auto count = getrandom(&random, sizeof(random), 0);
            if (count < 0 && errno == EINTR) continue;
            check(static_cast<int>(count), "temporary filename randomness");
            if (count != sizeof(random)) throw std::runtime_error("incomplete temporary filename randomness");
            temporary_ = ".hyprworks-" + std::to_string(random);
            fd_ = openat(parent_, temporary_.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            if (fd_ >= 0) return;
            if (errno != EEXIST) check(-1, "create temporary file");
        }
        throw std::runtime_error("temporary filename attempts exhausted");
    } catch (...) { close_fd(parent_); throw; }
}

FileWriter::~FileWriter() {
    if (fd_ >= 0) unlinkat(parent_, temporary_.c_str(), 0);
    close_fd(fd_); close_fd(parent_);
}

void FileWriter::append(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::write(fd_, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) check(-1, "write temporary file");
        if (!count) throw std::runtime_error("file write made no progress");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

void FileWriter::commit(bool replace) {
    check(fsync(fd_), "sync file");
    check(renameat2(parent_, temporary_.c_str(), parent_, destination_.c_str(), replace ? 0u : RENAME_NOREPLACE), "publish destination");
    close_fd(fd_);
    check(fsync(parent_), "file published, durability uncertain");
}

std::expected<void, std::string> write_file(const std::string& path, std::string_view contents, bool replace) {
    try {
        if (contents.size() > 16 * 1024 * 1024) throw std::runtime_error("file write exceeds 16 MiB");
        FileWriter file(path); file.append(contents); file.commit(replace); return {};
    } catch (const std::exception& e) { return std::unexpected(e.what()); }
}

std::expected<void, std::string> copy_file(const std::string& source, const std::string& destination, std::size_t limit) {
    int fd = -1;
    try {
        valid_path(source);
        fd = open(source.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); check(fd, "open copy source");
        struct stat st{}; check(fstat(fd, &st), "stat copy source");
        if (!S_ISREG(st.st_mode)) throw std::runtime_error("copy source must be a regular file");
        FileWriter file(destination);
        for (;;) {
            char data[65536]; const auto n = read(fd, data, sizeof(data));
            if (n < 0 && errno == EINTR) continue;
            check(static_cast<int>(n), "read copy source");
            if (!n) break;
            const auto size = static_cast<std::size_t>(n);
            if (size > limit) throw std::runtime_error("copy limit exceeded");
            file.append({data, size}); limit -= size;
        }
        close_fd(fd); file.commit(false); return {};
    } catch (const std::exception& e) { close_fd(fd); return std::unexpected(e.what()); }
}

std::expected<std::string, std::string> temporary(const std::string& parent, bool directory) {
    try {
        valid_path(parent);
        auto path = (std::filesystem::absolute(parent) / "hyprworks-XXXXXX").string();
        if (directory) { if (!mkdtemp(path.data())) check(-1, "create temporary directory"); }
        else { const int fd = mkostemp(path.data(), O_CLOEXEC); check(fd, "create temporary file"); close(fd); }
        return path;
    } catch (const std::exception& e) { return std::unexpected(e.what()); }
}

std::expected<void, std::string> make_directory(const std::string& path) {
    try {
        valid_path(path);
        if (mkdir(path.c_str(), 0700) < 0) {
            if (errno != EEXIST) check(-1, "create directory");
            const int fd = open(path.c_str(), O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_PATH);
            check(fd, "open existing directory"); close(fd);
        }
        return {};
    } catch (const std::exception& e) { return std::unexpected(e.what()); }
}

FileLock::~FileLock() { release(); }
void FileLock::release() noexcept { close_fd(fd_); }

std::expected<void, std::string> FileLock::acquire(const std::string& path) {
    if (fd_ >= 0) return std::unexpected("lock already held");
    int fd = -1;
    try {
        valid_path(path);
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600); check(fd, "open lock file");
        struct stat st{}; check(fstat(fd, &st), "stat lock file");
        if (!S_ISREG(st.st_mode)) throw std::runtime_error("lock requires a regular file");
        check(flock(fd, LOCK_EX | LOCK_NB), "acquire lock");
        fd_ = fd; return {};
    } catch (const std::exception& e) { close_fd(fd); return std::unexpected(e.what()); }
}

FileWatch::~FileWatch() { stop(); }
void FileWatch::stop() noexcept { close_fd(fd_); targets_.clear(); }
bool FileWatch::running() const noexcept { return fd_ >= 0; }
pollfd FileWatch::prepare_wait() const noexcept { return {fd_, POLLIN, 0}; }

std::expected<void, std::string> FileWatch::start(const std::string& path, bool directory) {
    stop();
    return add(path, directory);
}

std::expected<void, std::string> FileWatch::add(const std::string& path, bool directory) {
    try {
        valid_path(path);
        const auto target = std::filesystem::absolute(path).lexically_normal();
        const auto parent = directory ? target : target.parent_path();
        Target added{.path = target.string(), .name_offset = 0};
        added.name_offset = added.path.size() - (directory ? 0 : target.filename().native().size());
        for (const auto& existing : targets_)
            if (existing.path == added.path && existing.name_offset == added.name_offset) return {};
        if (targets_.size() == 256) throw std::runtime_error("file watch target limit reached (256)");
        if (targets_.size() == targets_.capacity())
            targets_.reserve(std::min<std::size_t>(256, std::max<std::size_t>(4, targets_.size() * 2)));
        if (fd_ < 0) {
            fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
            check(fd_, "create file watch");
        }
        added.descriptor = inotify_add_watch(fd_, parent.c_str(),
            IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
        check(added.descriptor, "watch directory");
        targets_.push_back(std::move(added));
        return {};
    } catch (const std::exception& error) {
        if (targets_.empty()) close_fd(fd_);
        return std::unexpected(error.what());
    }
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
            if (event->len)
                for (const auto& target : targets_)
                    if (target.descriptor == event->wd && (target.name_offset == target.path.size() ||
                        std::string_view(target.path).substr(target.name_offset) == event->name))
                        update.changed = true;
            offset += sizeof(inotify_event) + event->len;
        }
    }
    if (update.invalidated) stop();
    return update;
}

}
