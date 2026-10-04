/**
 * @file src/platform/linux/virtual_display_layout.h
 * @brief Place only Helios's virtual output outside the host desktop and behind the host's primary monitor.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace platf {
  /** @brief Logical geometry of an enabled output; never a requested physical mode. */
  struct output_rect_t {
    int x;  ///< Logical left edge.
    int y;  ///< Logical top edge.
    int width;  ///< Logical width, accounting for scale and rotation.
    int height;  ///< Logical height, accounting for scale and rotation.
  };

  /**
   * @brief Find a non-overlapping position adjacent to the rightmost host output.
   * @param outputs Existing enabled outputs, excluding Helios's virtual output.
   * @return Position for the virtual output, or no position without a valid host output.
   */
  inline std::optional<std::pair<int, int>> virtual_output_position(const std::vector<output_rect_t> &outputs) {
    std::optional<std::pair<int, int>> position;
    for (const auto &output : outputs) {
      if (output.width <= 0 || output.height <= 0) {
        continue;
      }
      const int right = output.x + output.width;
      if (!position || right > position->first) {
        position = std::pair {right, output.y};
      }
    }
    return position;
  }

  /**
   * @brief Choose an output priority that sorts the virtual output after every host output.
   *
   * KWin treats the lowest priority value as the primary monitor. The virtual output must never
   * become primary, otherwise panels, new windows and the game's monitor selection move to it.
   * Host priorities are left as they are; the virtual output only goes after all of them.
   *
   * @param host_priorities Current priorities of the enabled host outputs. Unknown values may be 0.
   * @return A priority larger than every host priority and than the number of host outputs.
   */
  inline std::uint32_t virtual_output_priority(const std::vector<std::uint32_t> &host_priorities) {
    // KWin numbers priorities from 1; counting the outputs covers hosts whose priority is unknown.
    std::uint32_t highest = static_cast<std::uint32_t>(host_priorities.size());
    for (const auto priority : host_priorities) {
      highest = std::max(highest, priority);
    }
    return highest + 1;
  }

  /**
   * @brief Move the named virtual output beside the host desktop and after every host output.
   *
   * Only the virtual output is written: its position and its priority. Physical modes, scales,
   * positions and priorities are left untouched.
   *
   * @param name Exact name of the output created by Helios's screencast connection.
   * @return True only after KWin acknowledges the change.
   */
  bool isolate_virtual_output(const std::string &name);
}  // namespace platf
