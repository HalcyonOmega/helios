/**
 * @file tests/unit/test_shutdown_loop.cpp
 * @brief Verify the tray's shutdown bridge with a bounded blocking loop and no live desktop.
 */
#include "src/shutdown_loop.h"

#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {
  /**
   * @brief Model a blocking event loop without requiring a compositor or a system tray.
   */
  struct blocking_loop_t {
    std::mutex mutex;  ///< Protect the loop state across the host and watcher threads.
    std::condition_variable changed;  ///< Signal loop entry and the thread-safe close request.
    bool entered = false;  ///< The event loop has begun processing.
    bool quit = false;  ///< The host's watcher requested that the loop close.
    bool timed_out = false;  ///< Fail the test instead of hanging if the bridge does not wake the loop.

    /**
     * @brief Block until the close request arrives, with a finite test deadline.
     * @return Nonzero to leave the event loop.
     */
    int process() {
      std::unique_lock lock(mutex);
      entered = true;
      changed.notify_all();
      timed_out = !changed.wait_for(lock, std::chrono::seconds(5), [this]() {
        return quit;
      });
      return 1;
    }

    /**
     * @brief Model the tray backend's thread-safe close request.
     */
    void wake() {
      std::scoped_lock lock(mutex);
      quit = true;
      changed.notify_all();
    }
  };
}  // namespace

TEST(ShutdownLoop, ShutdownBeforeLoopEntryWakesBlockingLoop) {
  auto event = std::make_shared<safe::event_t<bool>>();
  event->raise(true);
  blocking_loop_t loop;

  system_tray::detail::run_shutdown_loop(event, [&loop]() {
    return loop.process();
  },
                                         [&loop]() {
                                           loop.wake();
                                         });

  EXPECT_TRUE(loop.quit);
  EXPECT_FALSE(loop.timed_out);
}

TEST(ShutdownLoop, ServerFailureWakesAlreadyBlockedLoop) {
  auto event = std::make_shared<safe::event_t<bool>>();
  blocking_loop_t loop;
  std::jthread server([&loop, event]() {
    std::unique_lock lock(loop.mutex);
    if (loop.changed.wait_for(lock, std::chrono::seconds(5), [&loop]() {
          return loop.entered;
        })) {
      event->raise(true);
    }
  });

  system_tray::detail::run_shutdown_loop(event, [&loop]() {
    return loop.process();
  },
                                         [&loop]() {
                                           loop.wake();
                                         });

  EXPECT_TRUE(loop.entered);
  EXPECT_TRUE(loop.quit);
  EXPECT_FALSE(loop.timed_out);
}

TEST(ShutdownLoop, NormalLoopExitSignalsHostAndJoinsWatcher) {
  auto event = std::make_shared<safe::event_t<bool>>();
  int processed = 0;
  bool watcher_finished = false;

  system_tray::detail::run_shutdown_loop(event, [&processed]() {
    return ++processed < 2 ? 0 : 1;
  },
                                         [&watcher_finished]() {
                                           watcher_finished = true;
                                         });

  EXPECT_EQ(processed, 2);
  EXPECT_TRUE(event->peek());
  EXPECT_TRUE(watcher_finished);
}
