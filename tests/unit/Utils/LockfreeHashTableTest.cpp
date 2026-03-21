#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#define private public
#include "Utils/Parallel/lockfree/lockfree_hash_table.h"
#undef private

namespace {

Count_ptr::counter_type slotVersion(Count_ptr ptr) {
  return ptr.counter;
}

Hash_entry *slotPointer(Count_ptr ptr) {
  return ptr.ptr;
}

TEST(LockfreeHashTableTest, SupportsSingleThreadedInsertUpdateAndRemove) {
  Lockfree_hash_table table(32, 1);

  EXPECT_EQ(table.search(7, 0), std::make_pair(0, false));

  table.insert(7, 11, 0);
  EXPECT_EQ(table.search(7, 0), std::make_pair(11, true));

  table.insert(7, 17, 0);
  EXPECT_EQ(table.search(7, 0), std::make_pair(17, true));

  table.insert(13, 29, 0);
  EXPECT_EQ(table.search(13, 0), std::make_pair(29, true));

  table.remove(7, 0);
  EXPECT_EQ(table.search(7, 0), std::make_pair(0, false));
  EXPECT_EQ(table.search(13, 0), std::make_pair(29, true));
}

TEST(LockfreeHashTableTest, AllocatesStateForEachDeclaredThreadSlot) {
  Lockfree_hash_table table(64, 2);

  table.insert(21, 34, 1);
  EXPECT_EQ(table.search(21, 1), std::make_pair(34, true));

  table.remove(21, 1);
  EXPECT_EQ(table.search(21, 1), std::make_pair(0, false));
}

TEST(LockfreeHashTableTest, SupportsHighestDeclaredThreadSlot) {
  Lockfree_hash_table table(64, 256);

  table.insert(31, 47, 255);
  EXPECT_EQ(table.search(31, 255), std::make_pair(47, true));

  table.remove(31, 255);
  EXPECT_EQ(table.search(31, 255), std::make_pair(0, false));
}

TEST(LockfreeHashTableTest, SupportsNegativeKeysWithoutOutOfBoundsIndexing) {
  Lockfree_hash_table table(32, 1);

  table.insert(-7, 19, 0);
  EXPECT_EQ(table.search(-7, 0), std::make_pair(19, true));

  table.insert(-7, 23, 0);
  EXPECT_EQ(table.search(-7, 0), std::make_pair(23, true));

  table.remove(-7, 0);
  EXPECT_EQ(table.search(-7, 0), std::make_pair(0, false));
}

TEST(LockfreeHashTableTest, RejectsOutOfRangeThreadSlot) {
  Lockfree_hash_table table(32, 2);

  EXPECT_THROW(table.search(1, -1), std::out_of_range);
  EXPECT_THROW(table.insert(1, 2, 2), std::out_of_range);
  EXPECT_THROW(table.remove(1, 3), std::out_of_range);
}

TEST(LockfreeHashTableTest, InsertAndRemoveAdvanceSlotVersionCounters) {
  Lockfree_hash_table table(32, 1);
  const int key = 7;
  const int first_slot = table.hash1(key);

  const Count_ptr initial = table.loadTable(0, first_slot);
  ASSERT_EQ(slotPointer(initial), nullptr);

  table.insert(key, 11, 0);
  const Count_ptr inserted = table.loadTable(0, first_slot);
  ASSERT_NE(slotPointer(inserted), nullptr);
  EXPECT_EQ(slotPointer(inserted)->key, key);
  EXPECT_EQ(slotVersion(inserted),
            static_cast<Count_ptr::counter_type>(slotVersion(initial) + 1U));

  table.remove(key, 0);
  const Count_ptr removed = table.loadTable(0, first_slot);
  EXPECT_EQ(slotPointer(removed), nullptr);
  EXPECT_EQ(slotVersion(removed),
            static_cast<Count_ptr::counter_type>(slotVersion(inserted) + 1U));
}

TEST(LockfreeHashTableTest, CheckCounterHandlesVersionWraparound) {
  Lockfree_hash_table table(32, 1);
  const auto max_counter = std::numeric_limits<Count_ptr::counter_type>::max();

  EXPECT_TRUE(
      table.check_counter(max_counter - 1U, max_counter - 2U, 1U, 2U));
}

TEST(LockfreeHashTableTest, HelpRelocatePreservesFullWidthVersionCounters) {
  Lockfree_hash_table table(32, 1);
  const int key = 7;
  const int first_slot = table.hash1(key);
  const int second_slot = table.hash2(key);
  const Count_ptr::counter_type large_counter = 70000U;

  auto *src = new Hash_entry(key, 11);
  auto *dst = new Hash_entry(key + 1, 17);
  table.table[0][first_slot].store(Count_ptr{src, large_counter, true, {0, 0, 0}},
                                   std::memory_order_seq_cst);
  table.table[1][second_slot].store(Count_ptr{dst, 3U, false, {0, 0, 0}},
                                    std::memory_order_seq_cst);

  table.help_relocate(0, first_slot, true, 0);

  const Count_ptr after = table.loadTable(0, first_slot);
  EXPECT_EQ(slotPointer(after), src);
  EXPECT_FALSE(after.marked);
  EXPECT_EQ(slotVersion(after), large_counter + 1U);
}

TEST(LockfreeHashTableTest, ConcurrentSameKeyUpdatesRemainReachable) {
  Lockfree_hash_table table(32, 2);
  std::atomic<bool> start(false);

  auto worker = [&](int tid, int value) {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();

    for (int iteration = 0; iteration < 2000; ++iteration)
      table.insert(9, value, tid);
  };

  std::thread t0(worker, 0, 11);
  std::thread t1(worker, 1, 17);

  start.store(true, std::memory_order_release);
  t0.join();
  t1.join();

  const auto result = table.search(9, 0);
  EXPECT_TRUE(result.second);
  EXPECT_TRUE(result.first == 11 || result.first == 17);

  table.remove(9, 0);
  EXPECT_EQ(table.search(9, 0), std::make_pair(0, false));
}

TEST(LockfreeHashTableTest, InsertThrowsInsteadOfSilentlyDroppingWhenFull) {
  Lockfree_hash_table table(2, 1);

  table.insert(1, 10, 0);
  table.insert(2, 20, 0);

  EXPECT_THROW(table.insert(3, 30, 0), std::overflow_error);
  EXPECT_EQ(table.search(1, 0), std::make_pair(10, true));
  EXPECT_EQ(table.search(2, 0), std::make_pair(20, true));
  EXPECT_EQ(table.search(3, 0), std::make_pair(0, false));
}

} // namespace
