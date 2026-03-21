#include "Utils/Parallel/lockfree/lockfree_hash_table.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

#define THRESHOLD   50
#define R           25

// Inline bit twiddling functions
inline Count_ptr make_pointer(Hash_entry *e, std::uint16_t count,
                              bool marked = false) {
  return Count_ptr{e, count, marked, {0, 0, 0, 0, 0}};
}

inline Hash_entry *get_pointer(const Count_ptr &ptr) { return ptr.ptr; }

inline std::uint16_t get_counter(const Count_ptr &ptr) { return ptr.counter; }

inline bool get_marked(const Count_ptr &ptr) { return ptr.marked; }

inline Count_ptr set_marked(Count_ptr ptr, bool marked) {
  ptr.marked = marked;
  return ptr;
}

inline bool operator==(const Count_ptr &lhs, const Count_ptr &rhs) {
  return lhs.ptr == rhs.ptr && lhs.counter == rhs.counter &&
         lhs.marked == rhs.marked;
}

inline bool operator!=(const Count_ptr &lhs, const Count_ptr &rhs) {
  return !(lhs == rhs);
}

Lockfree_hash_table::Lockfree_hash_table(int capacity, int thread_count) {
  const int normalized_capacity = std::max(capacity, 2);
  size1 = normalized_capacity / 2;
  size2 = normalized_capacity - size1;

  table[0] = new std::atomic<Count_ptr>[size1];
  table[1] = new std::atomic<Count_ptr>[size2];
  for (int i = 0; i < size1; ++i)
    table[0][i].store(make_pointer(nullptr, 0), std::memory_order_relaxed);
  for (int i = 0; i < size2; ++i)
    table[1][i].store(make_pointer(nullptr, 0), std::memory_order_relaxed);

  this->thread_count = static_cast<std::size_t>(std::max(thread_count, 1));
  hp_rec = std::make_unique<HazardRecord[]>(this->thread_count);
  rlist.resize(this->thread_count);
}

Lockfree_hash_table::~Lockfree_hash_table() {

  for (int i = 0; i < size1; i++)
  {
    Hash_entry* node = get_pointer(loadTable(0, i));
    if (node != NULL)
      delete node;
  }
  
  for (int i = 0; i < size2; i++)
  {
    Hash_entry* node = get_pointer(loadTable(1, i));
    if (node != NULL)
      delete node;
  }

  for (std::size_t tid = 0; tid < rlist.size(); ++tid) {
    for (Hash_entry *Node : rlist[tid])
      delete Node;
  }

  delete[] table[0];
  delete[] table[1];
}

[[noreturn]] void rehash() {
  throw std::overflow_error(
      "lockfree hash table insert requires rehash, which is not implemented");
}
// HP functions
void Lockfree_hash_table::retire_node(Hash_entry* node, int tid) {
  auto &Retired = rlist[getThreadIndex(tid)];
  Retired.push_back(node);

  if (Retired.size() > R)
    scan(tid);
}

void Lockfree_hash_table::scan(int tid) {
  std::vector<Hash_entry*> plist;
  plist.reserve(thread_count * HAZARD_SLOT_COUNT);
  for (std::size_t i = 0; i < thread_count; ++i) {
    for (int j = 0; j < HAZARD_SLOT_COUNT; ++j) {
      Hash_entry *hptr = loadHazard(i, j);
      if (hptr != nullptr)
        plist.push_back(hptr);
    }
  }

  auto &Retired = rlist[getThreadIndex(tid)];
  std::vector<Hash_entry *> Remaining;
  Remaining.reserve(Retired.size());
  for (Hash_entry *Node : Retired) {
    if (std::find(plist.begin(), plist.end(), Node) != plist.end()) {
      Remaining.push_back(Node);
    } else {
      delete Node;
    }
  }
  Retired.swap(Remaining);
}

std::size_t Lockfree_hash_table::getThreadIndex(int tid) const {
  if (tid < 0 || static_cast<std::size_t>(tid) >= thread_count)
    throw std::out_of_range("invalid lockfree hash table thread id");
  return static_cast<std::size_t>(tid);
}

Count_ptr Lockfree_hash_table::loadTable(int which, int index) const {
  return table[which][index].load(std::memory_order_seq_cst);
}

bool Lockfree_hash_table::compareExchangeTable(int which, int index,
                                               Count_ptr &expected,
                                               Count_ptr desired) {
  return table[which][index].compare_exchange_strong(
      expected, desired, std::memory_order_seq_cst,
      std::memory_order_seq_cst);
}

void Lockfree_hash_table::publishHazard(int tid, int slot, Hash_entry *ptr) {
  hp_rec[getThreadIndex(tid)].slots[slot].store(ptr, std::memory_order_seq_cst);
}

Hash_entry *Lockfree_hash_table::loadHazard(std::size_t tid, int slot) const {
  return hp_rec[tid].slots[slot].load(std::memory_order_seq_cst);
}

void Lockfree_hash_table::clearHazards(int tid) {
  auto ThreadIndex = getThreadIndex(tid);
  for (int Slot = 0; Slot < HAZARD_SLOT_COUNT; ++Slot)
    hp_rec[ThreadIndex].slots[Slot].store(nullptr, std::memory_order_seq_cst);
}

// Private
int Lockfree_hash_table::hash1(int key) {
  uint32_t value = static_cast<uint32_t>(key);
  value = (value ^ 61U) ^ (value >> 16);
  value = value + (value << 3);
  value = value ^ (value >> 4);
  value = value * 0x27d4eb2dU;
  value = value ^ (value >> 15);
  return static_cast<int>(value % static_cast<uint32_t>(size1));
}

int Lockfree_hash_table::hash2(int key) {
  uint32_t value = static_cast<uint32_t>(key);
  value = ((value >> 16) ^ value) * 0x45d9f3bU;
  value = ((value >> 16) ^ value) * 0x45d9f3bU;
  value = (value >> 16) ^ value;
  return static_cast<int>(value % static_cast<uint32_t>(size2));
}

bool Lockfree_hash_table::check_counter(int ts1, int ts2, int ts1x, int ts2x) {
  return (ts1x >= ts1 + 2) && (ts2x >= ts2 + 2) && (ts2x >= ts1 + 3);
}

Find_result Lockfree_hash_table::find(int key, Count_ptr &ptr1, Count_ptr &ptr2, int tid) {
  int h1 = hash1(key);
  int h2 = hash2(key);

  while (true) {
    Find_result result = NIL;
    //std::cout << "Find inf loop" << std::endl;
    ptr1 = loadTable(0, h1);
    int ts1 = get_counter(ptr1);
    
    publishHazard(tid, 0, get_pointer(ptr1));
    if (get_pointer(ptr1) != get_pointer(loadTable(0, h1)))
      continue;

    if (get_pointer(ptr1)) {
      if (get_marked(ptr1)) {
        help_relocate(0, h1, false, tid);
        continue; 
      }

      if (get_pointer(ptr1)->key == key) 
        result = FIRST; 
    }

    ptr2 = loadTable(1, h2);
    int ts2 = get_counter(ptr2);

    publishHazard(tid, 1, get_pointer(ptr2));
    if (get_pointer(ptr2) != get_pointer(loadTable(1, h2)))
      continue;

    if (get_pointer(ptr2)) {
      if (get_marked(ptr2)) {
        help_relocate(1, h2, false, tid);
        continue; 
      }

      if (get_pointer(ptr2)->key == key) {
        if (result == FIRST) {
          del_dup(h1, ptr1, h2, ptr2, tid);
        } else {
          result = SECOND;
        }
      }
    }

    if (result == FIRST || result == SECOND) {
      return result;
    }

    ptr1 = loadTable(0, h1);
    ptr2 = loadTable(1, h2);

    if (check_counter(ts1, ts2, get_counter(ptr1), get_counter(ptr2))) {
      continue;
    } else {
      return NIL;
    }
  }
}

bool Lockfree_hash_table::relocate(int which, int index, int tid) {
try_again:
  int  route[THRESHOLD];
  Count_ptr pptr   = make_pointer(nullptr, 0);
  int  pre_idx     = 0;
  int  start_level = 0;
  int  tbl         = which;
  int  idx         = index;


path_discovery:
  bool found = false;
  int depth = start_level;
  do
  {
    Count_ptr ptr1 = loadTable(tbl, idx);
    
    while (get_marked(ptr1))
    {
      help_relocate(tbl, idx, false, tid);
      ptr1 = loadTable(tbl, idx);
    }

    Hash_entry* e1 = get_pointer(ptr1);
    Hash_entry* p1 = get_pointer(pptr);
    publishHazard(tid, 0, e1);
    if (e1 != get_pointer(loadTable(tbl, idx)))
      goto try_again;
    /*
    if (p1 && e1 && e1->key == p1->key)
    {
      if (tbl == 0)
        del_dup(idx, ptr1, pre_idx, pptr, tid);
      else
        del_dup(pre_idx, pptr, idx, ptr1, tid);
    }
    */
    if (e1 != nullptr)
    {
      route[depth] = idx;
      int key = e1->key; 
      pptr    = ptr1;
      pre_idx = idx;
      tbl     = 1 - tbl;
      idx     = (tbl == 0) ? hash1(key) : hash2(key); 
    }
    else
    {
      found = true;
    }
  } while (!found && ++depth < THRESHOLD);

  if (found)
  {
    tbl = 1 - tbl;
    for (int i = depth-1; i >= 0; i--, tbl = 1 - tbl)
    {
      idx = route[i];
      Count_ptr ptr1 = loadTable(tbl, idx);
      /*
      hp_rec[tid][0] = get_pointer(ptr1);
      if (get_pointer(ptr1) != get_pointer(table[tbl][idx]))
        goto try_again;
      */
      if (get_marked(ptr1))
      {
        help_relocate(tbl, idx, false, tid);
        ptr1 = loadTable(tbl, idx);
        publishHazard(tid, 0, get_pointer(ptr1));
        /*
        if (get_pointer(ptr1) != get_pointer(table[tbl][idx]))
          goto try_again;
         */
      }

      Hash_entry* e1 = get_pointer(ptr1);
      if (e1 == nullptr)
        continue;

      int dest_idx = (tbl == 0) ? hash2(e1->key) : hash1(e1->key);
      Count_ptr ptr2 = loadTable(1-tbl, dest_idx);
      Hash_entry* e2 = get_pointer(ptr2);

      if (e2 != nullptr)
      {
        start_level = i + 1;
        idx = dest_idx;
        tbl = 1 - tbl;
        goto path_discovery;
      }
      help_relocate(tbl, idx, true, tid);
    }
  }

  return found;
}

void Lockfree_hash_table::help_relocate(int which, int index, bool initiator, int tid) {
  while (1)
  {
    //std::cout << "help_relocate inf loop" << std::endl;
    Count_ptr ptr1 = loadTable(which, index);
    Hash_entry* src = get_pointer(ptr1);
    publishHazard(tid, 0, src);
    if (ptr1 != loadTable(which, index))
      continue;

    while (initiator && !get_marked(ptr1))
    {
      //std::cout << "help_relocate mark inf loop" << std::endl;
      if (src == nullptr)
        return;

      Count_ptr Expected = ptr1;
      compareExchangeTable(which, index, Expected, set_marked(ptr1, true));
      ptr1 = loadTable(which, index);
      src = get_pointer(ptr1);
      publishHazard(tid, 0, src);
      if (ptr1 != loadTable(which, index))
        continue;
    }

    if (!get_marked(ptr1))
      return;

    int hd = ((1 - which) == 0) ? hash1(src->key) : hash2(src->key);
    Count_ptr ptr2 = loadTable(1-which, hd);
    Hash_entry* dst = get_pointer(ptr2);
    publishHazard(tid, 1, dst);
    if (ptr2 != loadTable(1-which, hd))
      continue;

    uint16_t ts1 = get_counter(ptr1);
    uint16_t ts2 = get_counter(ptr2);

    if (dst == nullptr)
    {
      int nCnt = ts1 > ts2 ? ts1 + 1 : ts2 + 1;
      
      if (ptr1 != loadTable(which, index))
        continue;
      
      Count_ptr Expected = ptr2;
      if (compareExchangeTable(1-which, hd, Expected, make_pointer(src, nCnt)))
      {
        Count_ptr SourceExpected = ptr1;
        compareExchangeTable(which, index, SourceExpected,
                             make_pointer(nullptr, ts1+1));
        return;
      }
    }

    if (src == dst)
    {
      Count_ptr Expected = ptr1;
      compareExchangeTable(which, index, Expected,
                           make_pointer(nullptr, ts1+1));
      return;
    }

    Count_ptr Expected = ptr1;
    compareExchangeTable(which, index, Expected,
                         make_pointer(src, static_cast<std::uint16_t>(ts1 + 1)));
    return;
    
  }
}

void Lockfree_hash_table::del_dup(int idx1, Count_ptr ptr1, int idx2, Count_ptr ptr2, int tid) {
  Hash_entry *first = get_pointer(ptr1);
  Hash_entry *second = get_pointer(ptr2);
  publishHazard(tid, 0, first);
  publishHazard(tid, 1, second);
  if (ptr1 != loadTable(0, idx1) || ptr2 != loadTable(1, idx2))
    return;
  if (first == nullptr || second == nullptr || first->key != second->key)
    return;

  Count_ptr Expected = ptr2;
  if (compareExchangeTable(1, idx2, Expected,
                           make_pointer(nullptr, get_counter(ptr2) + 1))) {
    retire_node(second, tid);
  }
}
  
// Public
std::pair<int, bool> Lockfree_hash_table::search(int key, int tid) {
  clearHazards(tid);
  int h1 = hash1(key);
  int h2 = hash2(key);

  while (true) {
    //std::cout << "search inf loop " << key << std::endl;
    Count_ptr ptr1 = loadTable(0, h1); 
    Hash_entry *e1 = get_pointer(ptr1);
    
    publishHazard(tid, 0, e1);
    if (ptr1 != loadTable(0, h1))
      continue;

    int ts1 = get_counter(ptr1);

    if (e1 && e1->key == key) {
      auto Result =
          std::make_pair(e1->val.load(std::memory_order_relaxed), true);
      clearHazards(tid);
      return Result;
    }

    Count_ptr ptr2 = loadTable(1, h2);
    Hash_entry *e2 = get_pointer(ptr2);

    publishHazard(tid, 1, e2);
    if (ptr2 != loadTable(1, h2))
      continue;

    int ts2 = get_counter(ptr2);

    if (e2 && e2->key == key) {
      auto Result =
          std::make_pair(e2->val.load(std::memory_order_relaxed), true);
      clearHazards(tid);
      return Result;
    }

    int ts1x = get_counter(loadTable(0, h1));
    int ts2x = get_counter(loadTable(1, h2));

    if (check_counter(ts1, ts2, ts1x, ts2x))
      continue;
    else {
      clearHazards(tid);
      return std::make_pair(0, false);
    }
  }

  clearHazards(tid);
  return std::make_pair(0, false);
}

void Lockfree_hash_table::insert(int key, int val, int tid) {
  clearHazards(tid);
  Count_ptr ptr1, ptr2;

  Hash_entry *new_node = new Hash_entry(key, val);

  int h1 = hash1(key);
  int h2 = hash2(key);


  while (true) {
    //std::cout << "Inserting " << key << std::endl;
    Find_result result = find(key, ptr1, ptr2, tid);

    if (result == FIRST) {
      get_pointer(ptr1)->val.store(val, std::memory_order_relaxed);
      delete new_node;
      clearHazards(tid);
      return;
    }

    if (result == SECOND) {
      get_pointer(ptr2)->val.store(val, std::memory_order_relaxed);
      delete new_node;
      clearHazards(tid);
      return;
    }

    if (!get_pointer(ptr1)) { 
      Count_ptr Expected = ptr1;
      if (!compareExchangeTable(0, h1, Expected,
                                make_pointer(new_node, get_counter(ptr1) + 1))) {
        continue; 
      }
      clearHazards(tid);
      return;
    }

    if (!get_pointer(ptr2)) { 
      Count_ptr Expected = ptr2;
      if (!compareExchangeTable(1, h2, Expected,
                                make_pointer(new_node, get_counter(ptr2) + 1))) {
        continue; 
      }
      clearHazards(tid);
      return;
    }

    if (relocate(0, h1, tid)) {
      continue;
    } else {
      delete new_node;
      clearHazards(tid);
      rehash();
      return;
    }
  }
}

void Lockfree_hash_table::remove(int key, int tid) {
  clearHazards(tid);
  int h1 = hash1(key);
  int h2 = hash2(key);

  Count_ptr e1;
  Count_ptr e2;

  while (true) {
    //std::cout << "remove inf loop" << std::endl;
    Find_result ret = find(key, e1, e2, tid);

    if (ret == NIL) {
      clearHazards(tid);
      return;
    }

    if (ret == FIRST) {
      Count_ptr Expected = e1;
      if (compareExchangeTable(0, h1, Expected,
                               make_pointer(nullptr, get_counter(e1) + 1))) {
        retire_node(get_pointer(e1), tid);
        clearHazards(tid);
        return;
      }
    } else if (ret == SECOND) {
      if (loadTable(0, h1) != e1) 
        continue;
      Count_ptr Expected = e2;
      if (compareExchangeTable(1, h2, Expected,
                               make_pointer(nullptr, get_counter(e2) + 1))) {
        retire_node(get_pointer(e2), tid);
        clearHazards(tid);
        return;
      }
    }
  }
}
