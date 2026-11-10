#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace mooncake {

// Runs a callback on a dedicated thread when scheduled. Multiple Schedule()
// calls made before the worker consumes the pending request are coalesced into
// one callback invocation. Start() and Stop() must be called serially by the
// owner; Schedule() may be called concurrently from other threads.
class BackgroundWorker {
   public:
    using Callback = std::function<void()>;

    explicit BackgroundWorker(Callback callback)
        : callback_(std::move(callback)) {}

    ~BackgroundWorker() { Stop(); }

    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        requested_ = false;
        thread_ = std::thread(&BackgroundWorker::ThreadFunc, this);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !thread_.joinable()) {
                return;
            }
            running_ = false;
            requested_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Schedule() { ScheduleAfter(std::chrono::milliseconds::zero()); }

    // A retry does not busy-loop, delay a fresh request, or prevent Stop().
    void ScheduleAfter(std::chrono::milliseconds delay) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            const auto due = std::chrono::steady_clock::now() + delay;
            if (!requested_ || due < next_run_) {
                next_run_ = due;
            }
            requested_ = true;
        }
        cv_.notify_one();
    }

   private:
    void ThreadFunc() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_ || requested_; });
                if (!running_) {
                    return;
                }
                if (std::chrono::steady_clock::now() < next_run_) {
                    const auto due = next_run_;
                    cv_.wait_until(lock, due, [this, due] {
                        return !running_ || next_run_ < due;
                    });
                    continue;
                }
                requested_ = false;
            }
            callback_();
        }
    }

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_{false};
    bool requested_{false};
    std::chrono::steady_clock::time_point next_run_{};
};

}  // namespace mooncake
