/**
 * @file src/platform/linux/virtual_display_layout.h
 * @brief Place only Helios's virtual output outside the host desktop.
 */
#pragma once

#include <algorithm>
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
   * @brief Move only the named virtual output; leave physical modes, scales and priorities untouched.
   * @param name Exact name of the output created by Helios's screencast connection.
   * @return True only after KWin acknowledges the virtual-only position change.
   */
  bool position_virtual_output(const std::string &name);
}  // namespace platf
