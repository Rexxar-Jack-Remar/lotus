#pragma once

#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace lotus::cfl::dynamic_dyck::detail {
template <typename T1>

class In_FastDLL {
public:
  In_FastDLL() { dll_size = 0; }
  bool empty() const { return dll.empty(); }
  void add(T1 node) {

    typename std::list<T1>::iterator it;

    dll.push_back(node);
    it = dll.end();
    it--;
    nodemap[node] = it;
    dll_size++;
  }
  void remove(T1 node) {
    typename std::list<T1>::iterator it = nodemap[node];

    dll.erase(it);
    nodemap.erase(node);
    dll_size--;

    // stringmap[s] = dll.end();
  }

  int isInFDLL(T1 node) {
    if (nodemap.find(node) == nodemap.end())
      return 0;
    // if(stringmap[s] == dll.end() )
    // return 0;
    else
      return 1;
  }

  T1 front() { return dll.front(); }

  T1 front2() {
    typename std::list<T1>::iterator iter = dll.begin();
    iter++;
    return *iter;
  }
  void pop_front() {

    T1 dit = dll.front();
    nodemap.erase(dit);
    dll.pop_front();
    dll_size--;
  }
  unsigned size() { return dll_size; }

  void printlist() {

    std::cout << "==begin print list" << std::endl;
    for (typename std::list<T1>::iterator iter = dll.begin(); iter != dll.end();
         ++iter) {
      std::cout << *iter << std::endl;
    }
    std::cout << "==end" << std::endl;
  }

private:
  std::list<T1> dll;
  std::unordered_map<T1, typename std::list<T1>::iterator> nodemap;
  unsigned dll_size;
};

} // namespace lotus::cfl::dynamic_dyck::detail
