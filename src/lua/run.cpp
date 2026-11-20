#include "invocation.hpp"

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>

#ifdef HYPRWORKS_HAS_TOOLKIT
#include <hyprtoolkit/core/Timer.hpp>
#endif

namespace hyprworks::lua {
namespace {

timespec timeout_until(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now)
        return {};
    const auto remaining = std::chrono::ceil<std::chrono::nanoseconds>(deadline - now);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
    return {seconds.count(), (remaining - seconds).count()};
}

#ifdef HYPRWORKS_HAS_TOOLKIT
struct ToolkitLoop {
    Invocation& invocation;
    Hyprutils::Memory::CSharedPointer<Hyprtoolkit::IBackend> backend;
    Hyprutils::Memory::CAtomicSharedPointer<Hyprtoolkit::CTimer> timer;
    std::size_t registered = 0;
    WaitSet wait;
    std::exception_ptr failure;

    ~ToolkitLoop() {
        disarm();
        invocation.close();
    }

    void disarm() noexcept {
        for (std::size_t i = 0; i < registered; ++i) {
            const auto& fd = wait.fds[i];
            if (fd.fd >= 0)
                backend->removeFd(fd.fd);
        }
        registered = 0;
        if (timer)
            timer->cancel();
        timer.reset();
    }

    void arm() {
        wait = invocation.prepare_wait();
        for (; registered < wait.fds.size(); ++registered) {
            const auto& fd = wait.fds[registered];
            if (fd.fd < 0)
                continue;
            if (auto result = backend->addFd(fd.fd, [this] { dispatch(); }, fd.events); !result)
                throw std::runtime_error("register invocation descriptor: " + result.error());
        }
        if (wait.deadline) {
            const auto now = std::chrono::steady_clock::now();
            const auto timeout = *wait.deadline <= now ? std::chrono::steady_clock::duration::zero() : *wait.deadline - now;
            timer = backend->addTimer(timeout,
                [this](auto, void*) { dispatch(); }, nullptr);
            if (!timer)
                throw std::runtime_error("register invocation deadline: backend is closed");
        }
    }

    void dispatch() noexcept {
        try {
            const int result = ::poll(wait.fds.data(), wait.fds.size(), 0);
            const int error = errno;
            // Lua callbacks may close sockets and reuse their descriptor numbers.
            disarm();
            if (result >= 0) {
                if (auto dispatched = invocation.dispatch(wait); !dispatched)
                    throw std::runtime_error(dispatched.error());
            } else if (error != EINTR)
                throw std::system_error(error, std::generic_category(), "poll invocation");
            if (invocation.running()) {
                arm();
                return;
            }
        } catch (...) {
            failure = std::current_exception();
            disarm();
        }
        invocation.close();
    }
};
#endif

}

std::expected<void, std::string> Invocation::run() {
    try {
#ifdef HYPRWORKS_HAS_TOOLKIT
        if (state_->loop == EventLoop::toolkit) {
            ToolkitLoop host{.invocation = *this, .backend = {}, .timer = {}, .wait = {}, .failure = {}};
            if (!running())
                return {};
            host.backend = gui_backend(*state_);
            host.arm();
            host.backend->enterLoop();
            if (host.failure)
                std::rethrow_exception(host.failure);
            if (state_->gui_error)
                throw std::runtime_error(state_->gui_error);
            if (running())
                throw std::runtime_error("toolkit event loop stopped while the invocation was active");
        } else
#endif
        {
            while (running()) {
                auto wait = prepare_wait();
                const auto timeout = wait.deadline ? timeout_until(*wait.deadline) : timespec{};
                if (ppoll(wait.fds.data(), wait.fds.size(), wait.deadline ? &timeout : nullptr, nullptr) < 0) {
                    if (errno == EINTR)
                        continue;
                    throw std::system_error(errno, std::generic_category(), "ppoll");
                }
                if (auto dispatched = dispatch(wait); !dispatched)
                    return dispatched;
            }
        }
        close();
        return {};
    } catch (const std::exception& error) {
        close();
        return std::unexpected(error.what());
    }
}

}
