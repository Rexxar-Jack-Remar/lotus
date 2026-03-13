/**
 * @file MPIRankAnalysis.cpp
 * @brief Implementation of Symbolic MPI Rank Analysis
 */

#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Support/raw_ostream.h>
#include <deque>

using namespace llvm;
using namespace MPI;

namespace {

std::string normalizeMPIName(StringRef raw_name) {
  StringRef name = raw_name;
  if (name.startswith("\01")) {
    name = name.drop_front();
  }
  if (name.startswith("PMPI_")) {
    return ("MPI_" + name.drop_front(5)).str();
  }
  return name.str();
}

bool sameRankExpr(const RankExpr &lhs, const RankExpr &rhs) {
  return lhs.kind == rhs.kind &&
         lhs.concrete_value == rhs.concrete_value &&
         lhs.range_min == rhs.range_min &&
         lhs.range_max == rhs.range_max &&
         lhs.communicator == rhs.communicator;
}

bool sameRange(const std::pair<int, int> &lhs, const std::pair<int, int> &rhs) {
  return lhs.first == rhs.first && lhs.second == rhs.second;
}

} // namespace

bool RankExpr::mayEqual(const RankExpr &other) const {
  if (kind == Unknown || other.kind == Unknown) {
    return true; // Conservative
  }
  
  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }
  
  if (kind == Concrete && other.kind == Range) {
    return concrete_value >= other.range_min && 
           concrete_value <= other.range_max;
  }
  
  if (kind == Range && other.kind == Concrete) {
    return other.concrete_value >= range_min && 
           other.concrete_value <= range_max;
  }
  
  if (kind == Range && other.kind == Range) {
    // Ranges overlap
    return !(range_max < other.range_min || other.range_max < range_min);
  }
  
  // Symbolic ranks may be equal
  return true;
}

bool RankExpr::mustEqual(const RankExpr &other) const {
  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }
  
  // Cannot prove must-equality for symbolic/range/unknown
  return false;
}

MPIRankAnalysis::MPIRankAnalysis(Module &module) : m_module(module) {}

void MPIRankAnalysis::analyze() {
  errs() << "Starting MPI Rank Analysis...\n";

  m_value_to_rank.clear();
  m_inst_rank.clear();
  m_comm_size.clear();
  m_value_to_size_range.clear();

  identifyRankQueries();
  propagateValueFacts();
  propagateRankInfo();
  analyzeRankBranches();
  
  errs() << "MPI Rank Analysis Complete!\n";
}

void MPIRankAnalysis::identifyRankQueries() {
  // Find all MPI_Comm_rank and MPI_Comm_size calls
  
  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallBase *call = dyn_cast<CallBase>(&inst)) {
          Function *callee = call->getCalledFunction();
          if (!callee) continue;
          
          std::string normalized_name = normalizeMPIName(callee->getName());
          StringRef name = normalized_name;
          
          // MPI_Comm_rank(MPI_Comm comm, int *rank)
          if (name.equals("MPI_Comm_rank")) {
            if (call->arg_size() >= 2) {
              const Value *rank_ptr = call->getArgOperand(1);
              const Value *comm = call->getArgOperand(0);
              m_value_to_rank[rank_ptr] = RankExpr::makeSymbolic(comm);

              // Track loads from the output slot as symbolic rank values.
              for (const Use &use : rank_ptr->uses()) {
                if (LoadInst *load = dyn_cast<LoadInst>(use.getUser())) {
                  if (load->getPointerOperand() == rank_ptr) {
                    m_value_to_rank[load] = RankExpr::makeSymbolic(comm);
                  }
                }
              }
            }
          }
          
          // MPI_Comm_size(MPI_Comm comm, int *size)
          if (name.equals("MPI_Comm_size")) {
            if (call->arg_size() >= 2) {
              const Value *comm = call->getArgOperand(0);
              const Value *size_ptr = call->getArgOperand(1);

              m_comm_size[comm] = defaultCommSizeUpperBound();
              m_value_to_size_range[size_ptr] =
                  std::make_pair(1, defaultCommSizeUpperBound());
              for (const Use &use : size_ptr->uses()) {
                const auto *load = dyn_cast<LoadInst>(use.getUser());
                if (load && load->getPointerOperand() == size_ptr) {
                  m_value_to_size_range[load] =
                      std::make_pair(1, defaultCommSizeUpperBound());
                }
              }
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::propagateValueFacts() {
  bool changed = true;
  while (changed) {
    changed = false;

    for (Function &func : m_module) {
      for (BasicBlock &bb : func) {
        for (Instruction &inst : bb) {
          auto updateRank = [&](const Value *value, const RankExpr &expr) {
            auto it = m_value_to_rank.find(value);
            if (it != m_value_to_rank.end() && sameRankExpr(it->second, expr)) {
              return;
            }
            m_value_to_rank[value] = expr;
            changed = true;
          };

          auto updateSizeRange = [&](const Value *value,
                                     const std::pair<int, int> &range) {
            auto it = m_value_to_size_range.find(value);
            if (it != m_value_to_size_range.end() && sameRange(it->second, range)) {
              return;
            }
            m_value_to_size_range[value] = range;
            changed = true;
          };

          if (const auto *store = dyn_cast<StoreInst>(&inst)) {
            RankExpr expr = getRankExpr(store->getValueOperand());
            if (expr.kind != RankExpr::Unknown) {
              updateRank(store->getPointerOperand()->stripPointerCasts(), expr);
            }
            auto size_it = m_value_to_size_range.find(store->getValueOperand());
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(store->getPointerOperand()->stripPointerCasts(),
                              size_it->second);
            }
            continue;
          }

          if (const auto *load = dyn_cast<LoadInst>(&inst)) {
            RankExpr expr = getRankExpr(load->getPointerOperand()->stripPointerCasts());
            if (expr.kind != RankExpr::Unknown) {
              updateRank(load, expr);
            }
            auto size_it =
                m_value_to_size_range.find(load->getPointerOperand()->stripPointerCasts());
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(load, size_it->second);
            }
            continue;
          }

          if (const auto *phi = dyn_cast<PHINode>(&inst)) {
            bool saw_rank = false;
            RankExpr merged_rank;
            bool saw_size = false;
            std::pair<int, int> merged_size{0, 0};
            for (const Value *incoming : phi->incoming_values()) {
              RankExpr expr = getRankExpr(incoming);
              if (expr.kind != RankExpr::Unknown) {
                merged_rank = saw_rank ? mergeRankExpr(merged_rank, expr) : expr;
                saw_rank = true;
              }
              auto size_it = m_value_to_size_range.find(incoming);
              if (size_it != m_value_to_size_range.end()) {
                if (!saw_size) {
                  merged_size = size_it->second;
                  saw_size = true;
                } else {
                  merged_size.first = std::min(merged_size.first, size_it->second.first);
                  merged_size.second = std::max(merged_size.second, size_it->second.second);
                }
              }
            }
            if (saw_rank) {
              updateRank(phi, merged_rank);
            }
            if (saw_size) {
              updateSizeRange(phi, merged_size);
            }
            continue;
          }

          if (const auto *select = dyn_cast<SelectInst>(&inst)) {
            RankExpr lhs = getRankExpr(select->getTrueValue());
            RankExpr rhs = getRankExpr(select->getFalseValue());
            if (lhs.kind != RankExpr::Unknown || rhs.kind != RankExpr::Unknown) {
              updateRank(select, mergeRankExpr(lhs, rhs));
            }
            auto lhs_size = m_value_to_size_range.find(select->getTrueValue());
            auto rhs_size = m_value_to_size_range.find(select->getFalseValue());
            if (lhs_size != m_value_to_size_range.end() &&
                rhs_size != m_value_to_size_range.end()) {
              updateSizeRange(
                  select, std::make_pair(std::min(lhs_size->second.first,
                                                  rhs_size->second.first),
                                         std::max(lhs_size->second.second,
                                                  rhs_size->second.second)));
            }
            continue;
          }

          if (const auto *cast = dyn_cast<CastInst>(&inst)) {
            RankExpr expr = getRankExpr(cast->getOperand(0));
            if (expr.kind != RankExpr::Unknown) {
              updateRank(cast, expr);
            }
            auto size_it = m_value_to_size_range.find(cast->getOperand(0));
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(cast, size_it->second);
            }
            continue;
          }

          if (const auto *cb = dyn_cast<CallBase>(&inst)) {
            Function *callee = cb->getCalledFunction();
            if (!callee || callee->isDeclaration()) {
              continue;
            }

            for (unsigned arg_idx = 0; arg_idx < cb->arg_size() &&
                                      arg_idx < callee->arg_size();
                 ++arg_idx) {
              const Argument *formal = callee->getArg(arg_idx);
              RankExpr formal_rank = getRankExpr(formal);
              if (formal_rank.kind != RankExpr::Unknown) {
                updateRank(cb->getArgOperand(arg_idx)->stripPointerCasts(),
                           formal_rank);
              }
              auto formal_size = m_value_to_size_range.find(formal);
              if (formal_size != m_value_to_size_range.end()) {
                updateSizeRange(cb->getArgOperand(arg_idx)->stripPointerCasts(),
                                formal_size->second);
              }
            }

            RankExpr returned_rank;
            bool saw_returned_rank = false;
            std::pair<int, int> returned_size{0, 0};
            bool saw_returned_size = false;
            for (const BasicBlock &callee_bb : *callee) {
              if (const auto *ret =
                      dyn_cast<ReturnInst>(callee_bb.getTerminator())) {
                const Value *ret_val = ret->getReturnValue();
                if (!ret_val) {
                  continue;
                }
                RankExpr ret_expr = getRankExpr(ret_val);
                if (ret_expr.kind != RankExpr::Unknown) {
                  returned_rank = saw_returned_rank
                                      ? mergeRankExpr(returned_rank, ret_expr)
                                      : ret_expr;
                  saw_returned_rank = true;
                }
                auto ret_size = m_value_to_size_range.find(ret_val);
                if (ret_size != m_value_to_size_range.end()) {
                  if (!saw_returned_size) {
                    returned_size = ret_size->second;
                    saw_returned_size = true;
                  } else {
                    returned_size.first =
                        std::min(returned_size.first, ret_size->second.first);
                    returned_size.second =
                        std::max(returned_size.second, ret_size->second.second);
                  }
                }
              }
            }
            if (saw_returned_rank) {
              updateRank(cb, returned_rank);
            }
            if (saw_returned_size) {
              updateSizeRange(cb, returned_size);
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::propagateRankInfo() {
  m_inst_rank.clear();

  for (Function &func : m_module) {
    if (func.isDeclaration() || func.empty()) {
      continue;
    }

    std::map<const BasicBlock *, RankExpr> in_rank;
    std::deque<const BasicBlock *> worklist;
    std::set<const BasicBlock *> queued;

    const BasicBlock *entry = &func.getEntryBlock();
    in_rank[entry] = RankExpr::makeSymbolic();
    worklist.push_back(entry);
    queued.insert(entry);

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      queued.erase(bb);

      RankExpr current_rank = in_rank[bb];
      for (const Instruction &inst : *bb) {
        m_inst_rank[&inst] = current_rank;
      }

      const Instruction *terminator = bb->getTerminator();
      const auto *br = dyn_cast_or_null<BranchInst>(terminator);
      if (br) {
        for (unsigned succ_idx = 0; succ_idx < br->getNumSuccessors(); ++succ_idx) {
          const BasicBlock *succ = br->getSuccessor(succ_idx);
          RankExpr propagated = current_rank;
          RankExpr refined = current_rank;
          if (refineRankFromBranch(br, succ_idx, current_rank, refined)) {
            propagated = refined;
          }

          auto it = in_rank.find(succ);
          RankExpr merged =
              it == in_rank.end() ? propagated : mergeRankExpr(it->second, propagated);
          bool changed = it == in_rank.end() ||
                         merged.kind != it->second.kind ||
                         merged.concrete_value != it->second.concrete_value ||
                         merged.range_min != it->second.range_min ||
                         merged.range_max != it->second.range_max;
          if (!changed) {
            continue;
          }
          in_rank[succ] = merged;
          if (queued.insert(succ).second) {
            worklist.push_back(succ);
          }
        }
        continue;
      }

      for (const BasicBlock *succ : successors(bb)) {
        auto it = in_rank.find(succ);
        RankExpr merged =
            it == in_rank.end() ? current_rank : mergeRankExpr(it->second, current_rank);
        bool changed = it == in_rank.end() ||
                       merged.kind != it->second.kind ||
                       merged.concrete_value != it->second.concrete_value ||
                       merged.range_min != it->second.range_min ||
                       merged.range_max != it->second.range_max;
        if (!changed) {
          continue;
        }
        in_rank[succ] = merged;
        if (queued.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }
}

void MPIRankAnalysis::analyzeRankBranches() {
  // Analyze branches that depend on rank values
  // This enables precise analysis of rank-specific code paths
}

RankExpr MPIRankAnalysis::getRankExpr(const Value *val) const {
  auto it = m_value_to_rank.find(val);
  if (it != m_value_to_rank.end()) {
    return it->second;
  }
  return RankExpr(); // Unknown
}

RankExpr MPIRankAnalysis::getRankAtInstruction(const Instruction *inst) const {
  auto it = m_inst_rank.find(inst);
  if (it != m_inst_rank.end()) {
    return it->second;
  }
  return RankExpr::makeSymbolic(); // Conservative: any rank
}

bool MPIRankAnalysis::sameRank(const Instruction *i1, 
                                const Instruction *i2) const {
  RankExpr rank1 = getRankAtInstruction(i1);
  RankExpr rank2 = getRankAtInstruction(i2);
  
  return rank1.mustEqual(rank2);
}

std::set<int> MPIRankAnalysis::getPossibleRanks(const Instruction *inst) const {
  std::set<int> ranks;
  
  RankExpr rank = getRankAtInstruction(inst);
  
  if (rank.kind == RankExpr::Concrete) {
    ranks.insert(rank.concrete_value);
  } else if (rank.kind == RankExpr::Range) {
    for (int r = rank.range_min; r <= rank.range_max; ++r) {
      ranks.insert(r);
    }
  } else {
    // Symbolic or Unknown: conservatively return empty set
    // Caller should interpret this as "any rank"
  }
  
  return ranks;
}

bool MPIRankAnalysis::dependsOnRank(const Value *val) const {
  if (!val) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(val);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto rank_it = m_value_to_rank.find(current);
    if (rank_it != m_value_to_rank.end() &&
        rank_it->second.kind != RankExpr::Unknown) {
      return true;
    }

    auto size_it = m_value_to_size_range.find(current);
    if (size_it != m_value_to_size_range.end()) {
      return true;
    }

    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (unsigned i = 0; i < ce->getNumOperands(); ++i) {
        worklist.push_back(ce->getOperand(i));
      }
    }
  }

  return false;
}

RankExpr MPIRankAnalysis::mergeRankExpr(const RankExpr &lhs,
                                        const RankExpr &rhs) const {
  if (lhs.kind == RankExpr::Unknown) {
    return rhs;
  }
  if (rhs.kind == RankExpr::Unknown) {
    return lhs;
  }
  if (lhs.kind == RankExpr::Symbolic || rhs.kind == RankExpr::Symbolic) {
    return RankExpr::makeSymbolic(lhs.communicator ? lhs.communicator
                                                   : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Concrete && rhs.kind == RankExpr::Concrete) {
    if (lhs.concrete_value == rhs.concrete_value) {
      return lhs;
    }
    return RankExpr::makeSymbolic(lhs.communicator ? lhs.communicator
                                                   : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Range && rhs.kind == RankExpr::Range) {
    return RankExpr::makeRange(std::min(lhs.range_min, rhs.range_min),
                               std::max(lhs.range_max, rhs.range_max),
                               lhs.communicator ? lhs.communicator
                                                : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Range && rhs.kind == RankExpr::Concrete) {
    if (rhs.concrete_value >= lhs.range_min &&
        rhs.concrete_value <= lhs.range_max) {
      return lhs;
    }
    return RankExpr::makeRange(std::min(lhs.range_min, rhs.concrete_value),
                               std::max(lhs.range_max, rhs.concrete_value),
                               lhs.communicator ? lhs.communicator
                                                : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Concrete && rhs.kind == RankExpr::Range) {
    return mergeRankExpr(rhs, lhs);
  }
  return RankExpr::makeSymbolic(lhs.communicator ? lhs.communicator
                                                 : rhs.communicator);
}

bool MPIRankAnalysis::refineRankFromBranch(const BranchInst *br,
                                           unsigned succ_idx,
                                           RankExpr current,
                                           RankExpr &refined) const {
  if (!br || !br->isConditional()) {
    return false;
  }

  const auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
  if (!cmp) {
    return false;
  }

  const Value *lhs = cmp->getOperand(0);
  const Value *rhs = cmp->getOperand(1);
  const Value *rank_val = lhs;
  const Value *bound_val = rhs;
  ICmpInst::Predicate pred = cmp->getPredicate();
  if (!dependsOnRank(rank_val)) {
    rank_val = rhs;
    bound_val = lhs;
    pred = cmp->getSwappedPredicate();
  }

  if (!dependsOnRank(rank_val)) {
    return false;
  }

  const bool takes_edge = succ_idx == 0;
  refined =
      current.kind == RankExpr::Unknown ? RankExpr::makeSymbolic() : current;
  int bound_min = 0;
  int bound_max = defaultCommSizeUpperBound();
  if (!tryEvaluateIntRange(bound_val, bound_min, bound_max)) {
    return false;
  }

  int current_max = current.kind == RankExpr::Range ? current.range_max
                                                     : defaultCommSizeUpperBound();
  if (current_max < 0) {
    current_max = defaultCommSizeUpperBound();
  }

  switch (pred) {
  case CmpInst::ICMP_EQ:
    if (takes_edge && bound_min == bound_max) {
      refined = RankExpr::makeConcrete(bound_min, current.communicator);
      return true;
    }
    return false;
  case CmpInst::ICMP_NE:
    if (!takes_edge && bound_min == bound_max) {
      refined = RankExpr::makeConcrete(bound_min, current.communicator);
      return true;
    }
    return false;
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_ULT:
    if (takes_edge) {
      if (bound_max <= 0) {
        return false;
      }
      refined = RankExpr::makeRange(0, bound_max - 1, current.communicator);
    } else {
      refined = RankExpr::makeRange(std::max(0, bound_min), current_max,
                                    current.communicator);
    }
    return true;
  case CmpInst::ICMP_SLE:
  case CmpInst::ICMP_ULE:
    if (takes_edge) {
      refined = RankExpr::makeRange(0, bound_max, current.communicator);
    } else {
      refined =
          RankExpr::makeRange(std::max(0, bound_min + 1), current_max,
                              current.communicator);
    }
    return true;
  case CmpInst::ICMP_SGT:
  case CmpInst::ICMP_UGT:
    if (takes_edge) {
      refined = RankExpr::makeRange(std::max(0, bound_min + 1), current_max,
                                    current.communicator);
    } else {
      refined = RankExpr::makeRange(0, bound_max, current.communicator);
    }
    return true;
  case CmpInst::ICMP_SGE:
  case CmpInst::ICMP_UGE:
    if (takes_edge) {
      refined = RankExpr::makeRange(std::max(0, bound_min), current_max,
                                    current.communicator);
    } else {
      if (bound_max <= 0) {
        return false;
      }
      refined = RankExpr::makeRange(0, bound_max - 1, current.communicator);
    }
    return true;
  default:
    return false;
  }
}

bool MPIRankAnalysis::tryEvaluateIntRange(const Value *val, int &min_value,
                                          int &max_value) const {
  if (!val) {
    return false;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(val)) {
    min_value = ci->getSExtValue();
    max_value = min_value;
    return true;
  }

  auto size_it = m_value_to_size_range.find(val);
  if (size_it != m_value_to_size_range.end()) {
    min_value = size_it->second.first;
    max_value = size_it->second.second;
    return true;
  }

  const auto *inst = dyn_cast<Instruction>(val);
  if (!inst) {
    return false;
  }

  const auto *binop = dyn_cast<BinaryOperator>(inst);
  if (!binop) {
    return false;
  }

  int lhs_min = 0, lhs_max = 0, rhs_min = 0, rhs_max = 0;
  if (!tryEvaluateIntRange(binop->getOperand(0), lhs_min, lhs_max) ||
      !tryEvaluateIntRange(binop->getOperand(1), rhs_min, rhs_max)) {
    return false;
  }

  switch (binop->getOpcode()) {
  case Instruction::Add:
    min_value = lhs_min + rhs_min;
    max_value = lhs_max + rhs_max;
    return true;
  case Instruction::Sub:
    min_value = lhs_min - rhs_max;
    max_value = lhs_max - rhs_min;
    return true;
  default:
    return false;
  }
}
