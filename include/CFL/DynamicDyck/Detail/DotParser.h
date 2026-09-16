#pragma once

#include "CFL/DynamicDyck/Detail/Adjacency.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace lotus::cfl::dynamic_dyck::detail {
class SimpleDotParser {
public:
  SimpleDotParser() {};
  pair<string, string> ReturnNodePair(string &src, const string &delimiter);

  unsigned BuildNodeMap(const string &infile,
                        unordered_map<string, unsigned> &NodeID);

  void BuildMyHashTable(const string &infile,
                        unordered_map<string, unsigned> &NodeID,
                        unordered_map<string, unsigned> &EdgeID,
                        CFLHashMap &cm);

  void StripExtra(string &line) {
    line = line.substr(0, line.find_first_of("["));
  }
  int IsEdge(const string &line) {
    if (line.find("->") == string::npos)
      return 0;
    else
      return 1;
  }

  string GetEdgeLabel(const string &line) {
    size_t b, e;
    b = line.find_first_of("\"");
    e = line.find_last_of("\"");

    return line.substr(b + 1, (e - b - 1));
  }
};

} // namespace lotus::cfl::dynamic_dyck::detail
