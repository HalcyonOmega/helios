/**
 * @file tests/unit/test_virtual_display_layout.cpp
 * @brief Pure layout checks: virtual displays must not overlap host outputs or become primary.
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

TEST(VirtualDisplayLayout, SortsAfterTheHostPrimaryMonitor) {
  // KWin restored a saved layout where the virtual output was primary: host DP-1 reported 2.
  EXPECT_EQ(platf::virtual_output_priority({2}), 3u);
  EXPECT_EQ(platf::virtual_output_priority({1}), 2u);
}

TEST(VirtualDisplayLayout, SortsAfterEveryHostOutput) {
  EXPECT_EQ(platf::virtual_output_priority({3, 1, 2}), 4u);
  EXPECT_EQ(platf::virtual_output_priority({1, 7}), 8u);
}

TEST(VirtualDisplayLayout, SortsLastWhenHostPrioritiesAreUnknown) {
  // Compositors without the priority event leave every host value at 0; KWin numbers from 1.
  EXPECT_EQ(platf::virtual_output_priority({0, 0}), 3u);
  EXPECT_EQ(platf::virtual_output_priority({}), 1u);
}

#ifdef SUNSHINE_BUILD_KWIN
TEST(VirtualDisplayLayout, NeverConfiguresPhysicalOrUnownedOutputs) {
  // All calls reject before connecting to Wayland, even if a real desktop is available.
  EXPECT_FALSE(platf::isolate_virtual_output("DP-1"));
  EXPECT_FALSE(platf::isolate_virtual_output("Virtual-Moonlight"));
  EXPECT_FALSE(platf::isolate_virtual_output(""));
}
#endif
