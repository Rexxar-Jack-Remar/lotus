#include "Utils/Parallel/ThreadSafe.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using lotus::ThreadSafeMap;
using lotus::ThreadSafeSet;
using lotus::ThreadSafeVector;

TEST(ThreadSafeTest, SetSnapshotAndEraseReflectContents) {
  ThreadSafeSet<int> values;
  EXPECT_TRUE(values.insert(1));
  EXPECT_TRUE(values.insert(2));
  EXPECT_FALSE(values.insert(2));

  auto snapshot = values.snapshot();
  std::set<int> ordered(snapshot.begin(), snapshot.end());
  EXPECT_EQ(ordered, (std::set<int>{1, 2}));

  EXPECT_TRUE(values.erase(1));
  EXPECT_FALSE(values.contains(1));
  EXPECT_FALSE(values.erase(3));
}

TEST(ThreadSafeTest, MapSnapshotEraseAndUpdateWorkTogether) {
  ThreadSafeMap<std::string, int> values;
  EXPECT_TRUE(values.insert_or_assign("a", 1));
  EXPECT_FALSE(values.insert_or_assign("a", 2));
  EXPECT_TRUE(values.insert_or_assign("b", 3));

  EXPECT_TRUE(values.update("a", [](int &value) { value += 5; }));
  EXPECT_FALSE(values.update("missing", [](int &) {}));

  auto snapshot = values.snapshot();
  EXPECT_EQ(snapshot.at("a"), 7);
  EXPECT_EQ(snapshot.at("b"), 3);

  EXPECT_TRUE(values.erase("b"));
  EXPECT_FALSE(values.contains("b"));
  EXPECT_FALSE(values.erase("b"));
}

TEST(ThreadSafeTest, VectorSnapshotPreservesInsertionOrder) {
  ThreadSafeVector<int> values;
  values.push_back(4);
  values.push_back(5);
  values.push_back(6);

  EXPECT_EQ(values.snapshot(), (std::vector<int>{4, 5, 6}));
}

} // namespace
