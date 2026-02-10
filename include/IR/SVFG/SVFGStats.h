#pragma once

#include "IR/SVFG/SVFG.h"

#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace analysis {

class SVFGStats {
public:
  using SVFGNodeSet = std::set<const SVFGNode *>;
  using SVFGEdgeSet = std::set<const SVFGEdge *>;

  SVFGStats(SVFG *graph) : graph(graph) {}

  ~SVFGStats() = default;

  void performStat();
  void printStat(const std::string &title = "");

  void startTopLevelNodeTimer();
  void stopTopLevelNodeTimer();
  void startAddrTakenNodeTimer();
  void stopAddrTakenNodeTimer();
  void startDirVFEdgeTimer();
  void stopDirVFEdgeTimer();
  void startIndVFEdgeTimer();
  void stopIndVFEdgeTimer();
  void startOptTimer();
  void stopOptTimer();

  void addToForwardSlice(const SVFGNode *node);
  void addToBackwardSlice(const SVFGNode *node);
  bool inForwardSlice(const SVFGNode *node) const;
  bool inBackwardSlice(const SVFGNode *node) const;

  void addSource(const SVFGNode *node);
  void addSink(const SVFGNode *node);
  bool isSource(const SVFGNode *node) const;
  bool isSink(const SVFGNode *node) const;

  void performSCCAnalysis(const SVFGEdgeSet &insensitiveCalRetEdges);

private:
  void clear();
  void processGraph();
  void calculateNodeDegrees(const SVFGNode *node,
                            SVFGNodeSet &nodesWithIndInEdge,
                            SVFGNodeSet &nodesWithIndOutEdge);
  uint32_t getSCCRep(uint32_t nodeId);
  bool nodeInCycle(uint32_t nodeId);

  SVFG *graph;

  std::chrono::high_resolution_clock::time_point addTopLevelNodeTimeStart;
  std::chrono::high_resolution_clock::time_point addTopLevelNodeTimeEnd;
  std::chrono::high_resolution_clock::time_point addAddrTakenNodeTimeStart;
  std::chrono::high_resolution_clock::time_point addAddrTakenNodeTimeEnd;
  std::chrono::high_resolution_clock::time_point connectDirVFGEdgeTimeStart;
  std::chrono::high_resolution_clock::time_point connectDirVFGEdgeTimeEnd;
  std::chrono::high_resolution_clock::time_point connectIndVFGEdgeTimeStart;
  std::chrono::high_resolution_clock::time_point connectIndVFGEdgeTimeEnd;
  std::chrono::high_resolution_clock::time_point svfgOptTimeStart;
  std::chrono::high_resolution_clock::time_point svfgOptTimeEnd;

  int numNodes;
  int numFormalIn;
  int numFormalOut;
  int numFormalParam;
  int numFormalRet;
  int numActualIn;
  int numActualOut;
  int numActualParam;
  int numActualRet;
  int numLoad;
  int numStore;
  int numCopy;
  int numGep;
  int numAddr;
  int numMSSAPhi;
  int numPhi;

  int totalInEdge;
  int totalOutEdge;
  int totalIndInEdge;
  int totalIndOutEdge;
  int totalIndEdgeLabels;

  int totalIndCallEdge;
  int totalIndRetEdge;
  int totalDirCallEdge;
  int totalDirRetEdge;

  int avgInDegree;
  int avgOutDegree;
  uint32_t maxInDegree;
  uint32_t maxOutDegree;

  int avgIndInDegree;
  int avgIndOutDegree;
  uint32_t maxIndInDegree;
  uint32_t maxIndOutDegree;

  int avgWeight;

  SVFGNodeSet forwardSlice;
  SVFGNodeSet backwardSlice;
  SVFGNodeSet sources;
  SVFGNodeSet sinks;
};

} // namespace analysis
} // namespace lotus
