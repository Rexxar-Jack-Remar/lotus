/**
 * @file JoinTargetLocations.cpp
 * @brief Handle tracing and location resolution for join-target analysis
 */

#include "Concurrency/JoinTarget/JoinTargetAnalysis.h"

#include <deque>
#include <set>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace mhp {

namespace {

bool collectUniqueStoredValues(const Value *ptr,
                               SmallVectorImpl<const Value *> &storedValues) {
  if (!ptr) {
    return false;
  }

  std::set<const Value *> uniqueValues;
  for (const User *user : ptr->users()) {
    const auto *store = dyn_cast<StoreInst>(user);
    if (!store || store->getPointerOperand() != ptr) {
      continue;
    }
    uniqueValues.insert(store->getValueOperand());
    if (uniqueValues.size() > 1) {
      storedValues.clear();
      return false;
    }
  }

  for (const Value *value : uniqueValues) {
    storedValues.push_back(value);
  }
  return !storedValues.empty();
}

} // namespace

std::size_t HandleLocationHash::operator()(
    const HandleLocation &location) const {
  std::size_t seed = std::hash<const Value *>{}(location.base);
  seed ^= std::hash<bool>{}(location.is_base_wildcard) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  for (int64_t offset : location.offsets) {
    seed ^= std::hash<int64_t>{}(offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
  }
  return seed;
}

std::size_t SummaryLocationHash::operator()(
    const SummaryLocation &location) const {
  std::size_t seed = std::hash<unsigned>{}(location.arg_no);
  seed ^= std::hash<bool>{}(location.is_base_wildcard) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  for (int64_t offset : location.offsets) {
    seed ^= std::hash<int64_t>{}(offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
  }
  return seed;
}

const Value *JoinTargetAnalysis::traceThreadHandleRoot(const Value *value,
                                                       const Module *module) {
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolvedRoot = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      if (!resolvedRoot) {
        resolvedRoot = stripped;
      } else if (resolvedRoot != stripped) {
        return nullptr;
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      if (!module) {
        return stripped;
      }
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) {
            continue;
          }
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
          expanded = true;
        }

        for (const Function &func : *module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) {
                continue;
              }
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent) {
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
                expanded = true;
              }
            }
          }
        }
      }
      if (!expanded) {
        if (!resolvedRoot) {
          resolvedRoot = stripped;
        } else if (resolvedRoot != stripped) {
          return nullptr;
        }
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
      continue;
    }

    if (!resolvedRoot) {
      resolvedRoot = stripped;
    } else if (resolvedRoot != stripped) {
      return nullptr;
    }
  }

  return resolvedRoot;
}

void JoinTargetAnalysis::traceThreadHandleRoots(
    const Value *value, const Module *module,
    std::unordered_set<const Value *> &roots) {
  if (!value) {
    return;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      roots.insert(stripped);
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      const Function *parent = arg->getParent();
      if (module && parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) {
            continue;
          }
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
        }
        for (const Function &func : *module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) {
                continue;
              }
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent) {
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
              }
            }
          }
        }
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &op : inst->operands()) {
        worklist.push_back(op.get());
      }
      continue;
    }
  }
}

std::unordered_set<HandleLocation, HandleLocationHash>
JoinTargetAnalysis::resolveReadLocations(const Value *value) const {
  std::unordered_set<HandleLocation, HandleLocationHash> locations;
  if (!value) {
    return locations;
  }

  const Value *stripped = value->stripPointerCasts();
  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    locations.insert(HandleLocation{stripped, {}, false});
    return locations;
  }

  if (const auto *load = dyn_cast<LoadInst>(stripped)) {
    return resolveWriteLocations(load->getPointerOperand());
  }

  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    return resolveWriteLocations(gep);
  }

  if (const auto *phi = dyn_cast<PHINode>(stripped)) {
    for (const Value *incoming : phi->incoming_values()) {
      auto incomingLocations = resolveReadLocations(incoming);
      locations.insert(incomingLocations.begin(), incomingLocations.end());
    }
    return locations;
  }

  if (const auto *select = dyn_cast<SelectInst>(stripped)) {
    auto trueLocations = resolveReadLocations(select->getTrueValue());
    auto falseLocations = resolveReadLocations(select->getFalseValue());
    locations.insert(trueLocations.begin(), trueLocations.end());
    locations.insert(falseLocations.begin(), falseLocations.end());
    return locations;
  }

  return locations;
}

std::unordered_set<HandleLocation, HandleLocationHash>
JoinTargetAnalysis::resolveWriteLocations(const Value *value) const {
  std::unordered_set<HandleLocation, HandleLocationHash> locations;
  if (!value) {
    return locations;
  }

  const Value *stripped = value->stripPointerCasts();
  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    locations.insert(HandleLocation{stripped, {}, false});
    return locations;
  }

  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    auto baseLocations = resolveWriteLocations(gep->getPointerOperand());
    if (baseLocations.empty()) {
      if (const Value *baseRoot =
              traceThreadHandleRoot(gep->getPointerOperand(), nullptr)) {
        locations.insert(HandleLocation{baseRoot, {}, true});
      }
      return locations;
    }

    const DataLayout &layout = m_module.getDataLayout();
    APInt byteOffset(layout.getIndexTypeSizeInBits(gep->getType()), 0, true);
    const bool hasConstantOffset =
        gep->accumulateConstantOffset(layout, byteOffset) &&
        byteOffset.isSignedIntN(64);

    for (const HandleLocation &baseLocation : baseLocations) {
      HandleLocation location = baseLocation;
      if (!hasConstantOffset || location.is_base_wildcard) {
        location.offsets.clear();
        location.is_base_wildcard = true;
      } else {
        int64_t totalOffset = byteOffset.getSExtValue();
        if (!location.offsets.empty()) {
          totalOffset += location.offsets.front();
        }
        location.offsets.clear();
        if (totalOffset != 0) {
          location.offsets.push_back(totalOffset);
        }
      }
      locations.insert(std::move(location));
    }
    return locations;
  }

  if (const auto *phi = dyn_cast<PHINode>(stripped)) {
    for (const Value *incoming : phi->incoming_values()) {
      auto incomingLocations = resolveWriteLocations(incoming);
      locations.insert(incomingLocations.begin(), incomingLocations.end());
    }
    return locations;
  }

  if (const auto *select = dyn_cast<SelectInst>(stripped)) {
    auto trueLocations = resolveWriteLocations(select->getTrueValue());
    auto falseLocations = resolveWriteLocations(select->getFalseValue());
    locations.insert(trueLocations.begin(), trueLocations.end());
    locations.insert(falseLocations.begin(), falseLocations.end());
    return locations;
  }

  std::unordered_set<const Value *> roots;
  traceThreadHandleRoots(value, nullptr, roots);
  if (roots.empty()) {
    if (const Value *root = traceThreadHandleRoot(value, nullptr)) {
      locations.insert(HandleLocation{root, {}, true});
    }
  } else {
    for (const Value *root : roots) {
      locations.insert(HandleLocation{root, {}, true});
    }
  }
  return locations;
}

} // namespace mhp
