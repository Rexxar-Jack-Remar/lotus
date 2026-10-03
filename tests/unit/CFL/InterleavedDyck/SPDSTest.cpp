#include "SPDSTestSupport.h"
#include <gtest/gtest.h>
TEST(InterleavedDyckSPDS, SharedSuites) {
  for (const auto &test : lotus_spds_test::tests()) {
    SCOPED_TRACE(test.first);
    EXPECT_NO_THROW(test.second());
  }
}
