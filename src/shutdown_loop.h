/**
 * @file src/shutdown_loop.h
 * @brief Wake a blocking event loop when the host requests shutdown.
 */
#pragma once

#include "thread_safe.h"

#include <memory>
#include <thread>

namespace system_tray::detail {
  /**
   * @brief Bridge host shutdown and a blocking UI loop, joining the watcher before returning.
   * @param shutdown_event Host shutdown event, which may already be raised.
   * @param process Process UI events; return nonzero when the UI exits.
   * @param wake Thread-safe request to wake and close the UI loop.
   */
  template<class Process, class Wake>
  void run_shutdown_loop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event, Process process, Wake wake) {
    std::jthread shutdown_watcher([shutdown_event, wake]() {
      shutdown_event->view();
      wake();
    });
    while (process() == 0);
    shutdown_event->raise(true);
  }
}  // namespace system_tray::detail
