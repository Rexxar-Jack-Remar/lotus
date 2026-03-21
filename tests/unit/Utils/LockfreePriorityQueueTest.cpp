#include "Utils/Parallel/lockfree/mpmc/priority_queue.h"
#include "Utils/Parallel/lockfree/spsc/priority_queue.h"

#include <gtest/gtest.h>

namespace {

TEST(LockfreePriorityQueueTest, SpscPriorityQueuePopsHighestPriorityFirst) {
  lockfree::spsc::PriorityQueue<int, 8, 3> queue;

  EXPECT_TRUE(queue.Push(1, 0));
  EXPECT_TRUE(queue.Push(2, 1));
  EXPECT_TRUE(queue.Push(3, 2));

  int value = 0;
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 3);
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 2);
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 1);
  EXPECT_FALSE(queue.Pop(value));
}

TEST(LockfreePriorityQueueTest, MpmcPriorityQueuePopsHighestPriorityFirst) {
  lockfree::mpmc::PriorityQueue<int, 8, 3> queue;

  EXPECT_TRUE(queue.Push(4, 0));
  EXPECT_TRUE(queue.Push(5, 2));
  EXPECT_TRUE(queue.Push(6, 1));

  int value = 0;
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 5);
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 6);
  EXPECT_TRUE(queue.Pop(value));
  EXPECT_EQ(value, 4);
  EXPECT_FALSE(queue.Pop(value));
}

TEST(LockfreePriorityQueueTest, SpscPriorityQueueRejectsOutOfRangePriority) {
  lockfree::spsc::PriorityQueue<int, 8, 3> queue;

  EXPECT_FALSE(queue.Push(99, 3));
  int value = 0;
  EXPECT_FALSE(queue.Pop(value));
}

TEST(LockfreePriorityQueueTest, MpmcPriorityQueueRejectsOutOfRangePriority) {
  lockfree::mpmc::PriorityQueue<int, 8, 3> queue;

  EXPECT_FALSE(queue.Push(99, 3));
  int value = 0;
  EXPECT_FALSE(queue.Pop(value));
}

} // namespace
