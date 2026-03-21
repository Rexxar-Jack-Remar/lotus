#ifndef LOCKFREE_HASH_TABLE
#define LOCKFREE_HASH_TABLE

//#include "Utils/Parallel/lockfree/hash_table.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

struct Hash_entry {
  int key;
  std::atomic<int> val;

  Hash_entry() : key(0), val(0) {}
  Hash_entry(int entry_key, int entry_val) : key(entry_key), val(entry_val) {}
};

struct Count_ptr {
  Hash_entry *ptr;
  std::uint16_t counter;
  bool marked;
  std::uint8_t reserved[5];
};

static_assert(std::is_trivially_copyable<Count_ptr>::value,
              "Count_ptr must remain trivially copyable for atomic slots");

enum Find_result { FIRST, SECOND, NIL };

struct Lockfree_hash_table {
  Lockfree_hash_table(int capacity, int thread_count);
  ~Lockfree_hash_table();
  
  std::pair<int, bool> search(int key, int tid);
  void                 insert(int key, int val, int tid);
  void                 remove(int key, int tid);

private:
  static constexpr int HAZARD_SLOT_COUNT = 2;

  struct HazardRecord {
    std::atomic<Hash_entry *> slots[HAZARD_SLOT_COUNT];

    HazardRecord() {
      for (auto &Slot : slots)
        Slot.store(nullptr, std::memory_order_relaxed);
    }
  };

  std::atomic<Count_ptr> *table[2];
  int size1;
  int size2;

  std::vector<std::vector<Hash_entry *>> rlist;
  std::unique_ptr<HazardRecord[]> hp_rec;
  std::size_t thread_count;

  int hash1(int key);
  int hash2(int key);
  bool check_counter(int ts1, int ts2, int ts1x, int ts2x);
  std::size_t getThreadIndex(int tid) const;
  Count_ptr loadTable(int which, int index) const;
  bool compareExchangeTable(int which, int index, Count_ptr &expected,
                            Count_ptr desired);
  void publishHazard(int tid, int slot, Hash_entry *ptr);
  Hash_entry *loadHazard(std::size_t tid, int slot) const;
  void clearHazards(int tid);
  Find_result find(int key, Count_ptr &ptr1, Count_ptr &ptr2, int tid);
  bool relocate(int which, int index, int tid);
  void help_relocate(int which, int index, bool initiator, int tid);
  void del_dup(int idx1, Count_ptr ptr1, int idx2, Count_ptr ptr2, int tid);

  void retire_node(Hash_entry* node, int tid);
  void scan(int tid);
};
#endif
