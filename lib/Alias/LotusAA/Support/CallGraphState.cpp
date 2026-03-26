/// @file CallGraphState.cpp
/// @brief Call graph management with back-edge detection for recursive calls
///
/// Manages the **dynamic call graph** during on-the-fly call graph
/// construction. Tracks caller-callee relationships and identifies recursive
/// cycles (back-edges).
///
/// **Data Structure:**
/// ```
/// topDown_:  caller → set of callees (forward edges)
/// bottomUp_: callee → set of callers (reverse edges)
/// backEdges_: caller → set of callees that form cycles
/// ```
///
/// **Back-Edge Detection:**
/// Uses DFS to identify cycles in the call graph. Back-edges indicate
/// recursion:
/// ```
/// detectBackEdges():
///   for each root in call graph:
///     DFS from root:
///       if visit edge to node on DFS path:
///         mark as back-edge (recursion)
/// ```
///
/// **Usage in Analysis:**
/// - Bottom-up traversal uses topDown_ (callees before callers)
/// - Back-edges excluded from bottom-up ordering (handled specially)
/// - Callers of changed callees retrieved via bottomUp_
///
/// @see FunctionPointerResults for storing resolved call targets
/// @see InterProceduralPass.cpp for usage in fixpoint iteration

#include "Alias/LotusAA/Support/CallGraphState.h"

using namespace llvm;

void CallGraphState::clear() {
  topDown_.clear();
  bottomUp_.clear();
  backEdges_.clear();
}

FunctionSet &CallGraphState::getCallees(Function *func) {
  return topDown_[func];
}

const FunctionSet &CallGraphState::getCallees(Function *func) const {
  static const FunctionSet emptySet;
  auto it = topDown_.find(func);
  return (it != topDown_.end()) ? it->second : emptySet;
}

FunctionSet &CallGraphState::getCallers(Function *func) {
  return bottomUp_[func];
}

const FunctionSet &CallGraphState::getCallers(Function *func) const {
  static const FunctionSet emptySet;
  auto it = bottomUp_.find(func);
  return (it != bottomUp_.end()) ? it->second : emptySet;
}

void CallGraphState::addEdge(Function *caller, Function *callee) {
  if (caller && callee) {
    topDown_[caller].insert(callee);
    bottomUp_[callee].insert(caller);
  }
}

bool CallGraphState::isBackEdge(Function *caller, Function *callee) const {
  auto it = backEdges_.find(caller);
  return (it != backEdges_.end()) && it->second.count(callee);
}

void CallGraphState::markBackEdge(Function *caller, Function *callee) {
  backEdges_[caller].insert(callee);
}

void CallGraphState::initializeForFunctions(
    const std::vector<Function *> &functions) {
  for (Function *F : functions) {
    topDown_[F];  // Create empty entry
    bottomUp_[F]; // Create empty entry
  }
}

void CallGraphState::detectBackEdges(std::set<Function *> &changedFuncs) {
  std::map<Function *, FunctionSet, llvm_cmp> old_back_edges = backEdges_;
  backEdges_.clear();

  std::set<Function *, llvm_cmp> notVisited, visiting;

  // Initialize with all functions
  for (auto &item : topDown_) {
    notVisited.insert(item.first);
  }

  // DFS from each unvisited function
  while (!notVisited.empty()) {
    Function *func = *notVisited.begin();
    visiting.clear();
    detectBackEdgesRecursive(notVisited, visiting, func, changedFuncs);
  }

  std::set<Function *, llvm_cmp> affected;
  for (const auto &item : old_back_edges) {
    auto it = backEdges_.find(item.first);
    if (it == backEdges_.end() || it->second != item.second)
      affected.insert(item.first);
  }
  for (const auto &item : backEdges_) {
    auto it = old_back_edges.find(item.first);
    if (it == old_back_edges.end() || it->second != item.second)
      affected.insert(item.first);
  }
  changedFuncs.insert(affected.begin(), affected.end());
}

void CallGraphState::detectBackEdgesRecursive(
    std::set<Function *, llvm_cmp> &notVisited,
    std::set<Function *, llvm_cmp> &visiting, Function *currentFunc,
    std::set<Function *> &changed_funcs) {
  (void)changed_funcs;

  notVisited.erase(currentFunc);
  visiting.insert(currentFunc);

  // Visit all callees
  if (topDown_.count(currentFunc)) {
    for (Function *child : topDown_[currentFunc]) {
      if (notVisited.count(child)) {
        // Forward edge: recurse
        detectBackEdgesRecursive(notVisited, visiting, child, changed_funcs);
      } else if (visiting.count(child)) {
        // Back edge found: child is already on the DFS path
        backEdges_[currentFunc].insert(child);
      }
    }
  }

  visiting.erase(currentFunc);
}
