//===- DPItem.h -- Demand-driven analysis item (SVF-style) -------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DPItem / StmtDPItem: Demand-driven analysis item matching SVF's design.
// StmtDPItem(cur, loc) = (current pointer/object node ID, current SVFG location).
// Used by DDA to avoid recomputation and to handle cycles.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

struct SVFGNode;  // forward decl for StmtDPItem<SVFGNode>

/// Base DP item (current variable/location).
class DPItem {
protected:
  uint32_t cur;
  static uint32_t maximumBudget;

public:
  explicit DPItem(uint32_t c) : cur(c) {}
  DPItem(const DPItem &o) : cur(o.cur) {}
  uint32_t getCurNodeID() const { return cur; }
  void setCurNodeID(uint32_t c) { cur = c; }
  static void setMaxBudget(uint32_t max) { maximumBudget = max; }
  static uint32_t getMaxBudget() { return maximumBudget; }
  bool operator<(const DPItem &rhs) const { return cur < rhs.cur; }
  bool operator==(const DPItem &rhs) const { return cur == rhs.cur; }
  bool operator!=(const DPItem &rhs) const { return !(*this == rhs); }

  /// Debug dump (SVF-style).
  void dump(llvm::raw_ostream &os) const { os << "cur=" << cur; }
};

/// Flow-sensitive DP item: (current node ID, current SVFG location).
template <class LocCond>
class StmtDPItem : public DPItem {
protected:
  const LocCond *curloc;

public:
  StmtDPItem(uint32_t c, const LocCond *loc) : DPItem(c), curloc(loc) {}
  StmtDPItem(const StmtDPItem &o) : DPItem(o), curloc(o.curloc) {}
  const LocCond *getLoc() const { return curloc; }
  void setLoc(const LocCond *l) { curloc = l; }
  void setLocVar(const LocCond *l, uint32_t v) {
    curloc = l;
    cur = v;
  }
  bool operator<(const StmtDPItem &rhs) const {
    if (cur != rhs.cur)
      return cur < rhs.cur;
    return curloc < rhs.curloc;
  }
  bool operator==(const StmtDPItem &rhs) const {
    return cur == rhs.cur && curloc == rhs.curloc;
  }
  bool operator!=(const StmtDPItem &rhs) const { return !(*this == rhs); }

  /// Debug dump (SVF-style): cur and location pointer.
  void dump(llvm::raw_ostream &os) const {
    os << "cur=" << cur << " loc=" << static_cast<const void *>(curloc);
  }
};

} // namespace analysis
} // namespace lotus
