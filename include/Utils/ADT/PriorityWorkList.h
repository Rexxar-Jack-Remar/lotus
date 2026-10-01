#pragma once

#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_set>

namespace util {

// This worklist implementation is a priority queue with an additional set to
// help remove duplicate entries
template <typename T, typename Compare = std::less<T>,
          typename Hasher = std::hash<T>>
class PriorityWorkList {
public:
  using ElemType = T;

private:
  // The FIFO queue
  std::priority_queue<ElemType, std::vector<ElemType>, Compare> list;
  // Avoid duplicate entries in FIFO queue
  std::unordered_set<ElemType, Hasher> set;

public:
  PriorityWorkList() {}
  PriorityWorkList(const Compare &cmp) : list(cmp) {}

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
    ElemType ret = list.top();
    list.pop();
    set.erase(ret);
    return ret;
  }

  ElemType front() {
    assert(!list.empty() && "Trying to dequeue an empty queue!");
    return list.top();
  }

  bool empty() const { return list.empty(); }

  // Copy only the heap's contiguous values, never the duplicate-removal set.
  // Using the same priority_queue preserves the exact order of priority ties.
  std::vector<ElemType> peek(std::size_t limit) const {
    std::vector<ElemType> result;
    if (!limit)
      return result;
    auto pending = list;
    result.reserve(std::min(limit, pending.size()));
    while (!pending.empty() && result.size() < limit) {
      result.push_back(pending.top());
      pending.pop();
    }
    return result;
  }
};

} // namespace util
