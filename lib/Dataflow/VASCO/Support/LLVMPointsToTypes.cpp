#include "Dataflow/VASCO/Support/LLVMPointsToTypes.h"

#include <utility>

namespace vasco {
namespace llvmir {

namespace {

const MemoryLocationSet &emptyLocationSet() {
  static const MemoryLocationSet Empty;
  return Empty;
}

} // namespace

const MemoryLocationSet &
PointsToGraph::pointsTo(const PointsToValue &Value) const {
  auto It = Roots.find(Value);
  if (It == Roots.end()) {
    return emptyLocationSet();
  }
  return It->second;
}

const MemoryLocationSet &
PointsToGraph::load(const MemoryLocation &Location) const {
  auto It = Memory.find(Location);
  if (It == Memory.end()) {
    return emptyLocationSet();
  }
  return It->second;
}

void PointsToGraph::assign(const PointsToValue &Value,
                           const MemoryLocationSet &Targets) {
  if (Targets.empty()) {
    Roots.erase(Value);
    return;
  }
  Roots[Value] = Targets;
}

void PointsToGraph::erase(const PointsToValue &Value) { Roots.erase(Value); }

void PointsToGraph::clearReturnValue() { erase(PointsToValue::returnValue()); }

void PointsToGraph::store(const MemoryLocation &Location,
                          const MemoryLocationSet &Targets, bool StrongUpdate) {
  if (StrongUpdate) {
    if (Targets.empty()) {
      Memory.erase(Location);
    } else {
      Memory[Location] = Targets;
    }
    return;
  }

  auto Combined = load(Location);
  Combined.insert(Targets.begin(), Targets.end());
  if (Combined.empty()) {
    Memory.erase(Location);
  } else {
    Memory[Location] = std::move(Combined);
  }
}

void PointsToGraph::summarizeObject(const PointsToObject &Object) {
  std::vector<MemoryLocation> ToSummarize;
  for (const auto &Entry : Memory) {
    if (Entry.first.Object == Object) {
      ToSummarize.push_back(Entry.first);
    }
  }

  for (const auto &Location : ToSummarize) {
    auto Targets = load(Location);
    Targets.insert(MemoryLocation::summary(PointsToObject::summary()));
    Memory[Location] = std::move(Targets);
  }

  auto SummaryTargets = load(MemoryLocation::summary(Object));
  SummaryTargets.insert(MemoryLocation::summary(PointsToObject::summary()));
  Memory[MemoryLocation::summary(Object)] = std::move(SummaryTargets);
}

void PointsToGraph::unionRootsFrom(const PointsToGraph &Other) {
  for (const auto &Entry : Other.Roots) {
    auto Combined = pointsTo(Entry.first);
    Combined.insert(Entry.second.begin(), Entry.second.end());
    if (Combined.empty()) {
      Roots.erase(Entry.first);
    } else {
      Roots[Entry.first] = std::move(Combined);
    }
  }
}

void PointsToGraph::unionMemoryFrom(const PointsToGraph &Other) {
  for (const auto &Entry : Other.Memory) {
    auto Combined = load(Entry.first);
    Combined.insert(Entry.second.begin(), Entry.second.end());
    if (Combined.empty()) {
      Memory.erase(Entry.first);
    } else {
      Memory[Entry.first] = std::move(Combined);
    }
  }
}

void PointsToGraph::unionWith(const PointsToGraph &Other) {
  unionRootsFrom(Other);
  unionMemoryFrom(Other);
}

bool PointsToGraph::operator==(const PointsToGraph &Other) const {
  return Roots == Other.Roots && Memory == Other.Memory;
}

std::string formatAllocationSiteKind(AllocationSiteKind Kind) {
  switch (Kind) {
  case AllocationSiteKind::Stack:
    return "stack";
  case AllocationSiteKind::Heap:
    return "heap";
  case AllocationSiteKind::Global:
    return "global";
  case AllocationSiteKind::Function:
    return "function";
  case AllocationSiteKind::Argument:
    return "argument";
  case AllocationSiteKind::Summary:
    return "summary";
  }
  return "summary";
}

} // namespace llvmir
} // namespace vasco
