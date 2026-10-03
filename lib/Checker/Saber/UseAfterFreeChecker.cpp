#include "Checker/Saber/UseAfterFreeChecker.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/BugTypes.h"
#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberOptions.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <deque>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace analysis {
namespace {

bool mayReferToSameObject(const SVFG &svfg, const llvm::Value *left,
                          const llvm::Value *right) {
  const auto &lhs = svfg.getObjectIds(left);
  const auto &rhs = svfg.getObjectIds(right);
  // An absent fact is TOP in the shared SVFG importer, not a known empty set.
  if (lhs.empty() || rhs.empty()) return true;
  for (auto object : lhs)
    if (svfg.isUnknownObject(object) || rhs.count(object)) return true;
  for (auto object : rhs)
    if (svfg.isUnknownObject(object)) return true;
  return false;
}

ICFGNode *fragmentBefore(const llvm::Instruction &instruction, ICFG &icfg) {
  ICFGNode *fragment = icfg.getIntraBlockNode(instruction.getParent());
  for (const llvm::Instruction &earlier : *instruction.getParent()) {
    if (&earlier == &instruction) break;
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&earlier)) {
      if (call->doesNotReturn()) return nullptr;
      fragment = icfg.getRetICFGNode(call);
    }
  }
  return fragment;
}

struct State {
  const ICFGNode *node;
  std::vector<std::uintptr_t> calls;
  bool truncated = false;
};

std::unordered_set<const ICFGNode *>
reachableAfter(const llvm::CallInst &free, ICFG &icfg) {
  std::unordered_set<const ICFGNode *> reachable;
  std::set<std::tuple<std::uint64_t, std::vector<std::uintptr_t>, bool>> visited;
  std::deque<State> queue;
  queue.push_back({icfg.getRetICFGNode(&free), {}, false});
  const std::size_t limit = SaberCxtLimit;
  while (!queue.empty()) {
    State state = std::move(queue.front());
    queue.pop_front();
    if (!state.node) continue;
    auto key = std::make_tuple(state.node->getId(), state.calls, state.truncated);
    if (!visited.insert(std::move(key)).second) continue;
    reachable.insert(state.node);
    for (const ICFGEdge *edge : state.node->getOutEdges()) {
      State next = state;
      next.node = edge->getDstNode();
      if (edge->isCallCFGEdge()) {
        if (next.calls.size() >= limit) {
          if (!next.calls.empty()) next.calls.erase(next.calls.begin());
          next.truncated = true;
        }
        next.calls.push_back(reinterpret_cast<std::uintptr_t>(edge->getCallSite()));
      } else if (edge->isInterRetCFGEdge() && !next.calls.empty()) {
        if (next.calls.back() !=
            reinterpret_cast<std::uintptr_t>(edge->getCallSite())) continue;
        next.calls.pop_back();
      }
      // Empty stacks permit returns from the function containing the free.
      // A truncated prefix also permits older, unrecorded caller frames.
      queue.push_back(std::move(next));
    }
  }
  return reachable;
}

const llvm::Value *dereferencedPointer(const llvm::Instruction &instruction) {
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
    return load->getPointerOperand();
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
    return store->getPointerOperand();
  return nullptr;
}

} // namespace

void UseAfterFreeChecker::runOnModule(const llvm::Module &module,
                                      const SVFG &svfg, ICFG &icfg) const {
  BugReportMgr &reports = BugReportMgr::get_instance();
  const int bugType = reports.register_bug_type(
      "Use After Free", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-416");
  std::vector<const llvm::CallInst *> frees;
  std::vector<const llvm::Instruction *> dereferences;
  for (const llvm::Function &function : module) {
    if (function.isDeclaration()) continue;
    for (const llvm::BasicBlock &block : function)
      for (const llvm::Instruction &instruction : block) {
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          const llvm::Function *callee = call->getCalledFunction();
          if (callee && call->arg_size() &&
              SaberCheckerAPI::getCheckerAPI()->isMemDealloc(callee) &&
              !llvm::isa<llvm::ConstantPointerNull>(
                  call->getArgOperand(0)->stripPointerCasts()))
            frees.push_back(call);
        }
        if (dereferencedPointer(instruction))
          dereferences.push_back(&instruction);
      }
  }
  std::unordered_set<const llvm::Instruction *> reported;
  for (const llvm::CallInst *free : frees) {
    const auto reachable = reachableAfter(*free, icfg);
    for (const llvm::Instruction *use : dereferences) {
      if (reported.count(use)) continue;
      const llvm::Value *pointer = dereferencedPointer(*use);
      if (!mayReferToSameObject(svfg, free->getArgOperand(0), pointer)) continue;
      ICFGNode *target = fragmentBefore(*use, icfg);
      if (!target || !reachable.count(target)) continue;
      auto *report = new BugReport(bugType);
      report->append_step(const_cast<llvm::CallInst *>(free),
                          "Memory may be freed here");
      report->append_step(const_cast<llvm::Instruction *>(use),
                          "Memory may be dereferenced after free here");
      reports.insert_report(bugType, report, false);
      llvm::outs() << "Use After Free candidate at " << use->getFunction()->getName()
                   << '\n';
      reported.insert(use);
    }
  }
}

} // namespace analysis
} // namespace lotus
