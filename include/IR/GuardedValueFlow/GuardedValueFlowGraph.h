#pragma once

#include "IR/GuardedValueFlow/GuardedValueFlowNodes.h"
#include "IR/GuardedValueFlow/GuardedValueFlowSites.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Pass.h>

#include <map>
#include <memory>
#include <unordered_map>

namespace llvm {
namespace gvg {

class GuardedValueFlowGraph {
public:
  struct BlockCondition {
    GuardedValueFlowNode *condition_node{nullptr};
    BasicBlock *control_block{nullptr};
    BasicBlock *guard_successor{nullptr};
    ConditionRef condition;
    bool sense{true};
  };

  explicit GuardedValueFlowGraph(Function *base_function);
  ~GuardedValueFlowGraph() = default;

  Function *getBaseFunction() const { return base_function_; }

  template <typename NodeT, typename... Args> NodeT *createNode(Args &&...args) {
    auto node = std::make_unique<NodeT>(std::forward<Args>(args)...);
    NodeT *raw = node.get();
    raw->node_id_ = next_node_id_++;
    nodes_.push_back(std::move(node));
    assignNodeRegion(raw);
    return raw;
  }

  template <typename SiteT, typename... Args> SiteT *createSite(Args &&...args) {
    auto site = std::make_unique<SiteT>(std::forward<Args>(args)...);
    SiteT *raw = site.get();
    sites_.push_back(std::move(site));
    return raw;
  }

  GuardedValueFlowNode *findNode(Value *value) const;
  void mapValueNode(Value *value, GuardedValueFlowNode *node);

  GuardedValueFlowCallSite *findCallSite(Instruction *inst) const;
  void mapCallSite(Instruction *inst, GuardedValueFlowCallSite *site);

  GuardedValueFlowRegionNode *findRegion(BasicBlock *block) const;
  void mapRegion(BasicBlock *block, GuardedValueFlowRegionNode *node);
  GuardedValueFlowRegionNode *findUnitRegion(GuardedValueFlowNode *condition,
                                             bool sense) const;
  GuardedValueFlowRegionNode *
  findOrCreateUnitRegion(GuardedValueFlowNode *condition, bool sense,
                         BasicBlock *block, ConditionRef condition_ref);
  GuardedValueFlowRegionNode *
  findOrCreateAndRegion(GuardedValueFlowRegionNode *lhs,
                        GuardedValueFlowRegionNode *rhs, BasicBlock *block);
  GuardedValueFlowRegionNode *
  findOrCreateOrRegion(GuardedValueFlowRegionNode *lhs,
                       GuardedValueFlowRegionNode *rhs, BasicBlock *block);
  GuardedValueFlowRegionNode *
  findOrCreateNotRegion(GuardedValueFlowRegionNode *input, BasicBlock *block);
  GuardedValueFlowRegionNode *getAlwaysTrueRegion();
  GuardedValueFlowRegionNode *getAlwaysFalseRegion();
  GuardedValueFlowRegionNode *findSemanticRegion(path_cond_t path_cond) const;
  GuardedValueFlowRegionNode *findOrCreateSemanticRegion(path_cond_t path_cond,
                                                         BasicBlock *block);
  GuardedValueFlowNode *findSemanticConditionNode(path_cond_t path_cond) const;

  void addBlockCondition(BasicBlock *block, BlockCondition condition);
  ArrayRef<BlockCondition> getBlockConditions(BasicBlock *block) const;

  GuardedValueFlowNode *findLoadMemoryNode(Instruction *inst) const;
  void mapLoadMemoryNode(Instruction *inst, GuardedValueFlowNode *node);

  GuardedValueFlowNode *findStoreMemoryNode(Value *value,
                                            Instruction *inst) const;
  void mapStoreMemoryNode(Value *value, Instruction *inst,
                          GuardedValueFlowNode *node);
  GuardedValueFlowNode *findOrCreateStoreMemoryNode(
      Value *value, Instruction *inst, Type *type, BasicBlock *block,
      StringRef description = "store.mem");

  GuardedValueFlowReturnSite *findReturnSite(Instruction *inst) const;
  void mapReturnSite(Instruction *inst, GuardedValueFlowReturnSite *site);
  void refreshNodeRegions();

  ArrayRef<std::unique_ptr<GuardedValueFlowNode>> nodes() const { return nodes_; }
  ArrayRef<std::unique_ptr<GuardedValueFlowSite>> sites() const { return sites_; }

private:
  struct PointerPairLess {
    bool operator()(const std::pair<Value *, Instruction *> &lhs,
                    const std::pair<Value *, Instruction *> &rhs) const {
      return lhs < rhs;
    }
  };

  struct NodeBoolPairLess {
    bool operator()(const std::pair<GuardedValueFlowNode *, bool> &lhs,
                    const std::pair<GuardedValueFlowNode *, bool> &rhs) const {
      return lhs < rhs;
    }
  };

  struct RegionPairLess {
    bool operator()(const std::pair<GuardedValueFlowRegionNode *,
                                    GuardedValueFlowRegionNode *> &lhs,
                    const std::pair<GuardedValueFlowRegionNode *,
                                    GuardedValueFlowRegionNode *> &rhs) const {
      return lhs < rhs;
    }
  };

  Function *base_function_;
  unsigned next_node_id_{0};
  std::vector<std::unique_ptr<GuardedValueFlowNode>> nodes_;
  std::vector<std::unique_ptr<GuardedValueFlowSite>> sites_;
  DenseMap<Value *, GuardedValueFlowNode *> value_nodes_;
  DenseMap<Instruction *, GuardedValueFlowCallSite *> call_sites_;
  DenseMap<Instruction *, GuardedValueFlowReturnSite *> return_sites_;
  DenseMap<BasicBlock *, GuardedValueFlowRegionNode *> regions_;
  DenseMap<Instruction *, GuardedValueFlowNode *> load_memory_nodes_;
  DenseMap<BasicBlock *, std::vector<BlockCondition>> block_conditions_;
  DenseMap<path_cond_t, GuardedValueFlowRegionNode *> semantic_regions_;
  DenseMap<path_cond_t, GuardedValueFlowNode *> semantic_condition_nodes_;
  std::map<std::pair<Value *, Instruction *>, GuardedValueFlowNode *,
           PointerPairLess>
      store_memory_nodes_;
  std::map<std::pair<GuardedValueFlowNode *, bool>, GuardedValueFlowRegionNode *,
           NodeBoolPairLess>
      unit_regions_;
  std::map<std::pair<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>,
           GuardedValueFlowRegionNode *, RegionPairLess>
      and_regions_;
  std::map<std::pair<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>,
           GuardedValueFlowRegionNode *, RegionPairLess>
      or_regions_;
  DenseMap<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>
      not_regions_;
  GuardedValueFlowRegionNode *always_true_region_{nullptr};
  GuardedValueFlowRegionNode *always_false_region_{nullptr};

  void assignNodeRegion(GuardedValueFlowNode *node);
};

class GuardedValueFlowGraphBuilderPass : public ModulePass {
public:
  static char ID;

  GuardedValueFlowGraphBuilderPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
  StringRef getPassName() const override {
    return "GuardedValueFlowGraphBuilderPass";
  }

  bool hasGraphFor(const Function &F) const;
  GuardedValueFlowGraph &getGraph(const Function &F);

private:
  DenseMap<const Function *, std::unique_ptr<GuardedValueFlowGraph>> graphs_;

  std::unique_ptr<GuardedValueFlowGraph> buildGraph(Function &F);
};

ModulePass *createGuardedValueFlowGraphBuilderPass();

} // namespace gvg
} // namespace llvm
