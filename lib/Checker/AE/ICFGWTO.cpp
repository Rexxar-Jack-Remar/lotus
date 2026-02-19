#include "Checker/AE/ICFGWTO.h"

#include <llvm/IR/CFG.h>

namespace lotus {
namespace analysis {

std::vector<const llvm::BasicBlock *> ICFGSingletonWTO::getSuccessors() const {
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(bb), et = llvm::succ_end(bb); it != et;
       ++it) {
    succs.push_back(*it);
  }
  return succs;
}

std::vector<const llvm::BasicBlock *> ICFGCycleWTO::getSuccessors() const {
  std::vector<const llvm::BasicBlock *> succs;
  for (const auto *comp : components) {
    auto compSuccs = comp->getSuccessors();
    succs.insert(succs.end(), compSuccs.begin(), compSuccs.end());
  }
  return succs;
}

std::vector<const llvm::BasicBlock *>
ICFGCycleWTO::getExitSuccessors(const llvm::BasicBlock *exitBB) const {
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(exitBB), et = llvm::succ_end(exitBB);
       it != et; ++it) {
    const llvm::BasicBlock *succ = *it;
    bool inside = false;
    for (const auto *comp : components) {
      if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
        if (singleton->getBlock() == succ) {
          inside = true;
          break;
        }
      }
    }
    if (!inside) {
      succs.push_back(succ);
    }
  }
  return succs;
}

ICFGWTO::ICFGWTO(const llvm::Function *f) : func(f), entry(nullptr) {
  if (!f->empty()) {
    entry = &f->getEntryBlock();
    buildWTO();
  }
}

void ICFGWTO::buildWTO() {
  if (!entry)
    return;

  std::set<const llvm::BasicBlock *> visited;
  std::set<const llvm::BasicBlock *> inStack;

  const ICFGWTOComp *comp = buildComponent(entry, visited, inStack);
  if (comp) {
    components.push_back(comp);
  }
}

const ICFGWTOComp *
ICFGWTO::buildComponent(const llvm::BasicBlock *bb,
                        std::set<const llvm::BasicBlock *> &visited,
                        std::set<const llvm::BasicBlock *> &inStack) {
  if (!bb)
    return nullptr;

  if (visited.count(bb))
    return nullptr;

  visited.insert(bb);
  inStack.insert(bb);

  bool hasBackEdge = false;
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(bb), et = llvm::succ_end(bb); it != et;
       ++it) {
    const llvm::BasicBlock *succ = *it;
    succs.push_back(succ);
    if (inStack.count(succ)) {
      hasBackEdge = true;
    }
  }

  if (!hasBackEdge) {
    inStack.erase(bb);
    return new ICFGSingletonWTO(bb);
  }

  auto *cycle = new ICFGCycleWTO(bb);

  for (const llvm::BasicBlock *succ : succs) {
    if (inStack.count(succ)) {
      cycle->addComponent(new ICFGSingletonWTO(succ));
    } else if (!visited.count(succ)) {
      const ICFGWTOComp *subComp = buildComponent(succ, visited, inStack);
      if (subComp) {
        cycle->addComponent(subComp);
      }
    }
  }

  inStack.erase(bb);
  return cycle;
}

std::vector<const llvm::BasicBlock *>
ICFGWTO::getSuccessors(const llvm::BasicBlock *bb) const {
  for (const auto *comp : components) {
    if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
      if (singleton->getBlock() == bb) {
        return singleton->getSuccessors();
      }
    }
    if (const auto *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      for (const auto *innerComp : cycle->getComponents()) {
        if (const auto *innerSingleton =
                llvm::dyn_cast<ICFGSingletonWTO>(innerComp)) {
          if (innerSingleton->getBlock() == bb) {
            return innerSingleton->getSuccessors();
          }
        }
      }
    }
  }
  return {};
}

std::vector<const llvm::BasicBlock *> ICFGWTO::getNodes() const {
  std::vector<const llvm::BasicBlock *> nodes;
  std::set<const llvm::BasicBlock *> visited;

  std::function<void(const ICFGWTOComp *)> collect =
      [&](const ICFGWTOComp *comp) {
        if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
          if (visited.insert(singleton->getBlock()).second) {
            nodes.push_back(singleton->getBlock());
          }
        } else if (const auto *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
          for (const auto *innerComp : cycle->getComponents()) {
            collect(innerComp);
          }
        }
      };

  for (const auto *comp : components) {
    collect(comp);
  }

  return nodes;
}

} // namespace analysis
} // namespace lotus
