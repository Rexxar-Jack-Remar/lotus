#pragma once

#include "IR/GuardedValueFlow/ConditionRef.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace gvg {

class GuardedValueFlowGraph;
class GuardedValueFlowSite;

class AccessPath {
public:
  AccessPath() = default;
  AccessPath(Value *base, int64_t offset) : base_(base), offset_(offset) {}

  Value *getBase() const { return base_; }
  int64_t getOffset() const { return offset_; }
  bool empty() const { return base_ == nullptr && offset_ == 0; }

private:
  Value *base_{nullptr};
  int64_t offset_{0};
};

class GuardedValueFlowNode {
public:
  enum class Kind {
    CommonArgument,
    PseudoArgument,
    VariableArgument,
    CommonReturn,
    PseudoReturn,
    SimpleOperand,
    UndefValue,
    LoadMemory,
    StoreMemory,
    Phi,
    Region,
    CallSiteCommonOutput,
    CallSitePseudoOutput,
    CallSitePseudoInput,
    CallSiteArgumentSummary,
    CallSiteReturnSummary,
    SimpleOpcode,
    CastOpcode,
    Unknown,
  };

  struct Edge {
    GuardedValueFlowNode *target{nullptr};
    float confidence{1.0f};
    ConditionRef condition;
  };

  GuardedValueFlowNode(Kind kind, Type *type, GuardedValueFlowGraph *graph,
                       BasicBlock *block, Value *llvm_value = nullptr,
                       Instruction *dbg_inst = nullptr);
  virtual ~GuardedValueFlowNode() = default;

  Kind getKind() const { return kind_; }
  Type *getType() const { return type_; }
  GuardedValueFlowGraph *getGraph() const { return graph_; }
  BasicBlock *getParentBasicBlock() const { return block_; }
  Value *getLLVMValue() const { return llvm_value_; }
  Instruction *getDebugInstruction() const { return dbg_inst_; }
  unsigned getNodeId() const { return node_id_; }

  void addChild(GuardedValueFlowNode *child, float confidence = 1.0f,
                ConditionRef condition = ConditionRef::none());
  void clearChildren() { children_.clear(); }
  ArrayRef<Edge> children() const { return children_; }

  void addUseSite(GuardedValueFlowSite *site);
  ArrayRef<GuardedValueFlowSite *> useSites() const { return use_sites_; }

  void setDescription(std::string desc) { description_ = std::move(desc); }
  const std::string &getDescription() const { return description_; }

  void setAccessPath(AccessPath path) { access_path_ = path; }
  const AccessPath &getAccessPath() const { return access_path_; }

  void setIndex(unsigned idx) { index_ = idx; }
  unsigned getIndex() const { return index_; }

  void addMatchingCondition(GuardedValueFlowNode *producer,
                            ConditionRef condition);
  const std::multimap<GuardedValueFlowNode *, ConditionRef> &
  getMatchingConditions() const {
    return matching_conditions_;
  }

protected:
  Kind kind_;
  Type *type_;
  GuardedValueFlowGraph *graph_;
  BasicBlock *block_;
  Value *llvm_value_;
  Instruction *dbg_inst_;
  unsigned node_id_{0};
  unsigned index_{0};
  std::string description_;
  AccessPath access_path_;
  std::vector<Edge> children_;
  std::vector<GuardedValueFlowSite *> use_sites_;
  std::multimap<GuardedValueFlowNode *, ConditionRef> matching_conditions_;

  friend class GuardedValueFlowGraph;
};

class GuardedValueFlowRegionNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowRegionNode(Type *type, GuardedValueFlowGraph *graph,
                             BasicBlock *block, ConditionRef condition)
      : GuardedValueFlowNode(Kind::Region, type, graph, block, nullptr,
                             block ? block->getTerminator() : nullptr),
        region_condition_(condition) {}

  const ConditionRef &getRegionCondition() const { return region_condition_; }

private:
  ConditionRef region_condition_;
};

class GuardedValueFlowCallOutputNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallOutputNode(Kind kind, Type *type,
                                 GuardedValueFlowGraph *graph,
                                 BasicBlock *block, Value *llvm_value,
                                 Instruction *call_site,
                                 Function *callee = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value, call_site),
        call_site_(call_site), callee_(callee) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }

private:
  Instruction *call_site_;
  Function *callee_;
};

} // namespace gvg
} // namespace llvm
