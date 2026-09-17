#pragma once

#include "CFL/DynamicDyck/WeightedQuotient/Adjacency.h"
#include "CFL/DynamicDyck/WeightedQuotient/DisjointSet.h"
#include "CFL/DynamicDyck/WeightedQuotient/IndexedList.h"

#include <memory>
#include <unordered_set>

#include <sys/time.h>

namespace lotus::cfl::dynamic_dyck::weighted_quotient {
// Original global state is owned by one engine instance.
class Engine {
public:
  int no_weight = 0;
  int debug = 0;
  int usearray = 1;
  timeval begin_t{}, end_t{};
  unique_ptr<DisjointSet> resp;
  unordered_map<string, unsigned> weight;
  unordered_map<unsigned, unordered_set<string>> node2weight;
  int total_weight = 0;
  double elapsed = 0.0;
  size_t merge_count = 0, split_count = 0, cycle_rebuild_count = 0;
  size_t insertion_count = 0, deletion_count = 0, update_count = 0;
  unsigned final_edge_count = 0;
  vector<string> node_names;
  void inc_weight(unsigned i, unsigned j, unsigned eid, CFLHashMap &merged_cm,
                  unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes);
  list<unsigned> rmlistitemsin(list<unsigned> &lst, list<unsigned> &toremove);
  void dec_weight(unsigned i, unsigned j, unsigned eid, CFLHashMap &merged_cm,
                  unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes);
  void mainproc(In_FastDLL<string> &fdll,
                unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                CFLHashMap &cm, unordered_map<unsigned, list<unsigned>> &s_sets,
                unsigned NodeNum);
  void insert(CFLHashMap &cm, unsigned node_a, unsigned node_b, unsigned eid,
              CFLHashMap &merged_cm,
              unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
              unordered_map<unsigned, list<unsigned>> &s_sets);
  int split(list<unsigned> &nodes,
            unordered_map<unsigned, list<unsigned>> &s_sets, CFLHashMap &cm,
            CFLHashMap &merged_cm,
            unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes);
  bool weight_split_condition(unsigned noderesp);
  void split_further(CFLHashMap &cm, list<unsigned> tosplit,
                     CFLHashMap &merged_cm,
                     unordered_map<unsigned, list<unsigned>> &s_sets,
                     unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                     unordered_set<string> &checked);
  void split_further(CFLHashMap &cm, unsigned tosplit, CFLHashMap &merged_cm,
                     unordered_map<unsigned, list<unsigned>> &s_sets,
                     unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                     unordered_set<string> &checked);
  void deleteE(CFLHashMap &cm, unsigned u, unsigned v, unsigned l,
               CFLHashMap &merged_cm,
               unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
               unordered_map<unsigned, list<unsigned>> &s_sets);
  void arrayreach(CFLHashMap &cm,
                  unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                  unordered_map<unsigned, list<unsigned>> &s_sets);
  int arrayversion(bool inc_version, string init_dotfile, string input_seqfile);
  bool hasAffectedCycle(CFLHashMap &graph, unsigned source, unsigned target);
  void rebuild(CFLHashMap &original, CFLHashMap &merged,
               unordered_map<string, In_FastDLL<unsigned>> &colors,
               unordered_map<unsigned, list<unsigned>> &sets);
};
} // namespace lotus::cfl::dynamic_dyck::weighted_quotient
