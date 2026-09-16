#pragma once

#include <iostream>
#include <vector>

namespace lotus::cfl::dynamic_dyck::detail {

class DisjointSet {
public:
  std::vector<unsigned> arr;
  DisjointSet(unsigned size) {
    arr.reserve(size);
    for (unsigned i = 0; i < size; i++) {
      arr.push_back(i);
    }
  }

  unsigned find(unsigned idx) {
    // Preserve full path compression while avoiding stack overflow.
    unsigned root = idx;
    while (arr[root] != root)
      root = arr[root];
    while (arr[idx] != idx) {
      unsigned next = arr[idx];
      arr[idx] = root;
      idx = next;
    }
    return root;
  }

  // The first argument supplies the representative.
  void join(unsigned x, unsigned y) {
    unsigned xRoot = find(x);
    unsigned yRoot = find(y);
    arr[yRoot] = xRoot;
  }

  void print() {
    for (unsigned i = 0; i < arr.size(); i++) {
      std::cout << i << ", ";
    }
    std::cout << std::endl;
    for (unsigned i = 0; i < arr.size(); i++) {
      std::cout << find(i) << ", ";
    }
    std::cout << std::endl;
  }
};

} // namespace lotus::cfl::dynamic_dyck::detail
