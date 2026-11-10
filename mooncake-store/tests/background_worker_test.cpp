#include "background_worker.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

#include <gtest/gtest.h>

namespace mooncake::test {

TEST(BackgroundWorkerTest, CoalescesPendingSchedules) {
    std::mutex mutex;
    std::condition_variable cv;
    size_t callback_count = 0;
    bool release_first_callback = false;

    BackgroundWorker worker([&] {
        std::unique_lock<std::mutex> lock(mutex);
        ++callback_count;
        cv.notify_all();
        if (callback_count == 1) {
            cv.wait(lock, [&] { return release_first_callback; });
        }
    });
    worker.Start();
    worker.Schedule();

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1),
                                [&] { return callback_count == 1; }));
    }
    for (size_t i = 0; i < 10; ++i) {
        worker.Schedule();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_first_callback = true;
    }
    cv.notify_all();

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1),
                                [&] { return callback_count == 2; }));
    }
    worker.Stop();
    EXPECT_EQ(callback_count, 2);
}

TEST(BackgroundWorkerTest, ScheduleIsIgnoredWhileStopped) {
    std::mutex mutex;
    size_t callback_count = 0;
    BackgroundWorker worker([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++callback_count;
    });

    worker.Schedule();
    worker.Start();
    worker.Stop();
    worker.Schedule();

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(callback_count, 0);
}

TEST(BackgroundWorkerTest, ImmediateRequestPreemptsDelayedRetry) {
    std::promise<void> called;
    auto done = called.get_future();
    BackgroundWorker worker([&] { called.set_value(); });
    worker.Start();
    worker.ScheduleAfter(std::chrono::hours(1));
    worker.Schedule();
    EXPECT_EQ(done.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    worker.Stop();
}

TEST(BackgroundWorkerTest, StopCancelsDelayedRetry) {
    BackgroundWorker worker([] { ADD_FAILURE() << "Cancelled retry ran"; });
    worker.Start();
    worker.ScheduleAfter(std::chrono::hours(1));
    const auto start = std::chrono::steady_clock::now();
    worker.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              std::chrono::seconds(1));
}

TEST(BackgroundWorkerTest, CallbackCanScheduleItsOwnRetry) {
    std::promise<void> called;
    auto done = called.get_future();
    size_t attempts = 0;
    BackgroundWorker* self = nullptr;
    BackgroundWorker worker([&] {
        if (++attempts == 1) {
            self->ScheduleAfter(std::chrono::milliseconds(20));
        } else {
            called.set_value();
        }
    });
    self = &worker;
    worker.Start();
    worker.Schedule();
    EXPECT_EQ(done.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    worker.Stop();
    EXPECT_EQ(attempts, 2u);
}

}  // namespace mooncake::test
