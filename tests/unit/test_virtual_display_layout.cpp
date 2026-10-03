/**
 * @file tests/unit/test_virtual_display_layout.cpp
 * @brief Pure geometry checks: virtual displays must not overlap host outputs.
 */
#include "../tests_common.h"
#include "src/platform/linux/virtual_display_layout.h"

TEST(VirtualDisplayLayout, PreservesUltrawideDesktop) {
  const auto position = platf::virtual_output_position({{0, 0, 5120, 1440}});
  ASSERT_TRUE(position);
  EXPECT_EQ(position->first, 5120);
  EXPECT_EQ(position->second, 0);
}

TEST(VirtualDisplayLayout, AttachesToRightmostOfMultipleScaledOutputs) {
  // Input rectangles are already logical coordinates, not physical pixel modes.
  const auto position = platf::virtual_output_position({{-1920, -400, 1920, 1080}, {0, 0, 2560, 720}, {2560, 100, 1080, 1920}});
  ASSERT_TRUE(position);
  EXPECT_EQ(position->first, 3640);
  EXPECT_EQ(position->second, 100);
}

TEST(VirtualDisplayLayout, HandlesNegativeDesktopCoordinates) {
  const auto position = platf::virtual_output_position({{-5120, -1440, 5120, 1440}});
  ASSERT_TRUE(position);
  EXPECT_EQ(position->first, 0);
  EXPECT_EQ(position->second, -1440);
}

TEST(VirtualDisplayLayout, RejectsMissingOrInvalidHostGeometry) {
  EXPECT_FALSE(platf::virtual_output_position({}));
  EXPECT_FALSE(platf::virtual_output_position({{0, 0, 0, 1440}, {0, 0, 1920, -1}}));
}

#ifdef SUNSHINE_BUILD_KWIN
TEST(VirtualDisplayLayout, NeverConfiguresPhysicalOrUnownedOutputs) {
  // All calls reject before connecting to Wayland, even if a real desktop is available.
  EXPECT_FALSE(platf::position_virtual_output("DP-1"));
  EXPECT_FALSE(platf::position_virtual_output("Virtual-Moonlight"));
  EXPECT_FALSE(platf::position_virtual_output(""));
}
#endif
