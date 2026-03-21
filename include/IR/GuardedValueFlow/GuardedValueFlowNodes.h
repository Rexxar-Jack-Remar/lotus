#pragma once

#include "IR/GuardedValueFlow/ConditionRef.h"

#include <llvm/ADT/ArrayRef.h>
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
class GuardedValueFlowReturnSite;

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

class GuardedValueFlowArgumentNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowArgumentNode(Kind kind, Type *type,
                               GuardedValueFlowGraph *graph,
                               BasicBlock *block, Value *llvm_value)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast<Instruction>(llvm_value)) {}

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonArgument ||
           node->getKind() == Kind::PseudoArgument ||
           node->getKind() == Kind::VariableArgument;
  }
};

class GuardedValueFlowRegionNode : public GuardedValueFlowNode {
public:
  enum class Form {
    AlwaysTrue,
    AlwaysFalse,
    Unit,
    And,
    Or,
    Not,
  };

  GuardedValueFlowRegionNode(Type *type, GuardedValueFlowGraph *graph,
                             BasicBlock *block, Form form,
                             GuardedValueFlowNode *condition_node,
                             bool condition_sense, ConditionRef condition)
      : GuardedValueFlowNode(Kind::Region, type, graph, block, nullptr,
                             block ? block->getTerminator() : nullptr),
        form_(form), condition_node_(condition_node),
        condition_sense_(condition_sense), region_condition_(condition) {}

  Form getForm() const { return form_; }
  bool isAlwaysTrue() const { return form_ == Form::AlwaysTrue; }
  bool isAlwaysFalse() const { return form_ == Form::AlwaysFalse; }
  bool isCompound() const {
    return form_ == Form::And || form_ == Form::Or || form_ == Form::Not;
  }
  GuardedValueFlowNode *getConditionNode() const { return condition_node_; }
  bool getConditionSense() const { return condition_sense_; }
  const ConditionRef &getRegionCondition() const { return region_condition_; }

private:
  Form form_;
  GuardedValueFlowNode *condition_node_{nullptr};
  bool condition_sense_{true};
  ConditionRef region_condition_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Region;
  }
};

class GuardedValueFlowOpcodeNode : public GuardedValueFlowNode {
public:
  enum class OpcodeKind {
    Invalid,
    URem,
    FRem,
    SRem,
    UDiv,
    SDiv,
    FDiv,
    And,
    Or,
    Xor,
    Shl,
    LShr,
    AShr,
    Mul,
    FMul,
    FAdd,
    FSub,
    Add,
    Sub,
    AddrSpaceCast,
    IntToPtr,
    PtrToInt,
    BitCast,
    ZExt,
    SExt,
    Trunc,
    FPTrunc,
    FPExt,
    SIToFP,
    FPToSI,
    UIToFP,
    FPToUI,
    ExtractElement,
    InsertElement,
    GetElementPtr,
    Select,
    ICmp,
    FCmp,
    Concat,
  };

  GuardedValueFlowOpcodeNode(Kind kind, Type *type,
                             GuardedValueFlowGraph *graph, BasicBlock *block,
                             OpcodeKind opcode_kind)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, nullptr),
        opcode_kind_(opcode_kind) {}

  OpcodeKind getOpcodeKind() const { return opcode_kind_; }
  void setCmpPredicate(int predicate) { cmp_predicate_ = predicate; }
  int getCmpPredicate() const { return cmp_predicate_; }
  void setCastWidths(uint64_t src_bits, uint64_t dst_bits) {
    cast_src_bits_ = src_bits;
    cast_dst_bits_ = dst_bits;
  }
  uint64_t getCastSrcBits() const { return cast_src_bits_; }
  uint64_t getCastDstBits() const { return cast_dst_bits_; }
  void setIntConstant(int64_t value) {
    has_int_constant_ = true;
    int_constant_ = value;
  }
  bool hasIntConstant() const { return has_int_constant_; }
  int64_t getIntConstant() const { return int_constant_; }

private:
  OpcodeKind opcode_kind_;
  int cmp_predicate_{-1};
  uint64_t cast_src_bits_{0};
  uint64_t cast_dst_bits_{0};
  bool has_int_constant_{false};
  int64_t int_constant_{0};

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::SimpleOpcode ||
           node->getKind() == Kind::CastOpcode;
  }
};

class GuardedValueFlowPhiNode : public GuardedValueFlowNode {
public:
  struct Incoming {
    GuardedValueFlowNode *value_node{nullptr};
    BasicBlock *incoming_block{nullptr};
    GuardedValueFlowNode *condition_node{nullptr};
    bool condition_sense{true};
    ConditionRef condition;
  };

  GuardedValueFlowPhiNode(Type *type, GuardedValueFlowGraph *graph,
                          BasicBlock *block, Value *llvm_value,
                          Instruction *dbg_inst)
      : GuardedValueFlowNode(Kind::Phi, type, graph, block, llvm_value,
                             dbg_inst) {}

  void addIncoming(GuardedValueFlowNode *value_node, BasicBlock *incoming_block,
                   GuardedValueFlowNode *condition_node, bool condition_sense,
                   ConditionRef condition);
  ArrayRef<Incoming> incoming() const { return incoming_; }

private:
  std::vector<Incoming> incoming_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Phi;
  }
};

class GuardedValueFlowReturnNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowReturnNode(Kind kind, Type *type, GuardedValueFlowGraph *graph,
                             BasicBlock *block, Value *llvm_value = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast_or_null<Instruction>(llvm_value)) {}

  void addReturnValueSitePair(GuardedValueFlowNode *value_node,
                              GuardedValueFlowReturnSite *site);
  GuardedValueFlowReturnSite *
  getReturnSite(const GuardedValueFlowNode *value_node) const;

private:
  std::map<const GuardedValueFlowNode *, GuardedValueFlowReturnSite *>
      return_sites_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonReturn ||
           node->getKind() == Kind::PseudoReturn;
  }
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

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteCommonOutput ||
           node->getKind() == Kind::CallSitePseudoOutput ||
           node->getKind() == Kind::CallSitePseudoInput;
  }
};

class GuardedValueFlowCallSummaryNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallSummaryNode(Kind kind, Type *type,
                                  GuardedValueFlowGraph *graph,
                                  BasicBlock *block, Instruction *call_site,
                                  Function *callee, unsigned summary_index)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, call_site),
        call_site_(call_site), callee_(callee), summary_index_(summary_index) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }
  unsigned getSummaryIndex() const { return summary_index_; }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteArgumentSummary ||
           node->getKind() == Kind::CallSiteReturnSummary;
  }

private:
  Instruction *call_site_;
  Function *callee_;
  unsigned summary_index_{0};
};

} // namespace gvg
} // namespace llvm
