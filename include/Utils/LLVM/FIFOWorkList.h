#pragma once

#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_set>
#include <vector>

namespace util {

// This worklist implementation is a simple FIFO queue with an additional set to
// help remove duplicate entries
template <typename T, typename Hasher = std::hash<T>> class FIFOWorkList {
public:
  using ElemType = T;

private:
  // The FIFO queue
  std::queue<ElemType> list;
  // Avoid duplicate entries in FIFO queue
  std::unordered_set<ElemType, Hasher> set;

public:
  FIFOWorkList() {}
  bool enqueue(ElemType elem) {
    if (!set.count(elem)) {
      list.push(elem);
      set.insert(elem);
      return true;
    } else
      return false;
  }

  ElemType dequeue() {
    assert(!list.empty() && "Trying to dequeue an empty queue!");
    ElemType ret = list.front();
    list.pop();
    set.erase(ret);
    return ret;
  }

  ElemType front() {
    assert(!list.empty() && "Trying to dequeue an empty queue!");
    return list.front();
  }

  bool empty() const { return list.empty(); }

  // Preview values without copying the duplicate-removal hash table.
  std::vector<ElemType> peek(std::size_t limit) const {
    std::vector<ElemType> result;
    if (!limit)
      return result;
    auto pending = list;
    result.reserve(std::min(limit, pending.size()));
    while (!pending.empty() && result.size() < limit) {
      result.push_back(pending.front());
      pending.pop();
    }
    return result;
  }
};

} // namespace util
