/*
 * LotusAA - Type Definitions and Utilities
 * 
 * Common types, type aliases, and comparators used throughout LotusAA.
 * Provides LLVM-compatible data structures and helper types.
 */

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace llvm {

class PathCond {
public:
  struct Literal {
    enum class Kind {
      Value,
      Branch,
      SwitchCase,
      SwitchDefault,
      InvokeNormal,
      InvokeUnwind,
      Block,
      CallTarget,
      Imported,
      Opaque,
    };

    Kind kind;
    Value *value;
    BasicBlock *block;
    BasicBlock *successor;
    Function *callee;
    ConstantInt *case_value;
    const PathCond *opaque;

    Literal(Kind kind = Kind::Opaque, Value *value = nullptr,
            BasicBlock *block = nullptr, BasicBlock *successor = nullptr,
            Function *callee = nullptr, ConstantInt *case_value = nullptr,
            const PathCond *opaque = nullptr)
        : kind(kind), value(value), block(block), successor(successor),
          callee(callee), case_value(case_value), opaque(opaque) {}

    bool operator<(const Literal &other) const {
      return std::tie(kind, value, block, successor, callee, case_value,
                      opaque) <
             std::tie(other.kind, other.value, other.block, other.successor,
                      other.callee, other.case_value, other.opaque);
    }
  };

  struct ConstraintSummary {
    bool always_true = false;
    bool always_false = false;
    std::set<Literal> positive_literals;
    std::set<Literal> negative_literals;
  };

  enum class Kind {
    True,
    False,
    ValueAtom,
    BranchAtom,
    SwitchCaseAtom,
    SwitchDefaultAtom,
    InvokeNormalAtom,
    InvokeUnwindAtom,
    BlockAtom,
    CallTargetAtom,
    ImportedAtom,
    Not,
    And,
    Or,
  };

private:
  Kind kind_;
  Value *value_;
  BasicBlock *block_;
  BasicBlock *successor_;
  Function *callee_;
  Function *owner_func_;
  ConstantInt *case_value_;
  PathCond *imported_;
  bool sense_;
  PathCond *lhs_;
  PathCond *rhs_;
  ConstraintSummary summary_;

  explicit PathCond(Kind kind)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), owner_func_(nullptr), case_value_(nullptr),
        imported_(nullptr),
        sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, bool sense)
      : kind_(Kind::ValueAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(nullptr), owner_func_(nullptr),
        case_value_(nullptr), imported_(nullptr), sense_(sense), lhs_(nullptr),
        rhs_(nullptr) {}

  PathCond(Kind kind, BasicBlock *block, BasicBlock *successor, Value *value,
           bool sense, Function *owner_func = nullptr,
           ConstantInt *case_value = nullptr, PathCond *imported = nullptr,
           Function *callee = nullptr)
      : kind_(kind), value_(value), block_(block), successor_(successor),
        callee_(callee), owner_func_(owner_func), case_value_(case_value),
        imported_(imported), sense_(sense), lhs_(nullptr), rhs_(nullptr) {}

  explicit PathCond(BasicBlock *block)
      : kind_(Kind::BlockAtom), value_(nullptr), block_(block),
        successor_(nullptr), callee_(nullptr),
        owner_func_(block ? block->getParent() : nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, Function *callee)
      : kind_(Kind::CallTargetAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(callee), owner_func_(nullptr),
        case_value_(nullptr), imported_(nullptr), sense_(true),
        lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Kind kind, PathCond *lhs, PathCond *rhs)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), owner_func_(nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(lhs), rhs_(rhs) {}

  static Function *inferOwner(Value *value) {
    if (auto *inst = dyn_cast_or_null<Instruction>(value))
      return inst->getFunction();
    if (auto *arg = dyn_cast_or_null<Argument>(value))
      return arg->getParent();
    return nullptr;
  }

  bool addLiteral(const Literal &literal, bool positive) {
    auto &dst = positive ? summary_.positive_literals : summary_.negative_literals;
    auto &other =
        positive ? summary_.negative_literals : summary_.positive_literals;
    if (other.count(literal) != 0) {
      summary_.always_false = true;
      summary_.always_true = false;
      summary_.positive_literals.clear();
      summary_.negative_literals.clear();
      return false;
    }
    dst.insert(literal);
    return true;
  }

  void initializeAtomicSummary() {
    switch (kind_) {
    case Kind::True:
      summary_.always_true = true;
      break;
    case Kind::False:
      summary_.always_false = true;
      break;
    case Kind::ValueAtom:
      addLiteral(Literal(Literal::Kind::Value, value_), sense_);
      break;
    case Kind::BranchAtom:
      addLiteral(Literal(Literal::Kind::Branch, value_, block_, nullptr),
                 sense_);
      break;
    case Kind::SwitchCaseAtom:
      addLiteral(Literal(Literal::Kind::SwitchCase, value_, block_, successor_,
                         nullptr, case_value_),
                 true);
      break;
    case Kind::SwitchDefaultAtom:
      addLiteral(Literal(Literal::Kind::SwitchDefault, value_, block_,
                         successor_),
                 true);
      break;
    case Kind::InvokeNormalAtom:
      addLiteral(Literal(Literal::Kind::InvokeNormal, nullptr, block_,
                         successor_),
                 true);
      break;
    case Kind::InvokeUnwindAtom:
      addLiteral(Literal(Literal::Kind::InvokeUnwind, nullptr, block_,
                         successor_),
                 true);
      break;
    case Kind::BlockAtom:
      addLiteral(Literal(Literal::Kind::Block, nullptr, block_), true);
      break;
    case Kind::CallTargetAtom:
      addLiteral(
          Literal(Literal::Kind::CallTarget, value_, nullptr, nullptr, callee_),
          true);
      break;
    case Kind::ImportedAtom:
      addLiteral(
          Literal(Literal::Kind::Imported, nullptr, nullptr, nullptr, nullptr,
                  nullptr, imported_),
          true);
      break;
    case Kind::Not:
    case Kind::And:
    case Kind::Or:
      break;
    }
  }

  void initializeNotSummary() {
    if (!lhs_)
      return;

    if (lhs_->summary_.always_true) {
      summary_.always_false = true;
      return;
    }

    if (lhs_->summary_.always_false) {
      summary_.always_true = true;
      return;
    }

    switch (lhs_->getKind()) {
    case Kind::ValueAtom:
      addLiteral(Literal(Literal::Kind::Value, lhs_->getValue()),
                 !lhs_->getSense());
      return;
    case Kind::BranchAtom:
      addLiteral(Literal(Literal::Kind::Branch, lhs_->getValue(),
                         lhs_->getBlock(), nullptr),
                 !lhs_->getSense());
      return;
    case Kind::Not:
      summary_ = lhs_->getLhs() ? lhs_->getLhs()->getConstraintSummary()
                                : ConstraintSummary{};
      return;
    default:
      addLiteral(Literal(Literal::Kind::Opaque, nullptr, nullptr, nullptr,
                         nullptr, nullptr, lhs_),
                 false);
      return;
    }
  }

  void initializeAndSummary() {
    if (!lhs_ || !rhs_) {
      summary_.always_false = true;
      return;
    }

    if (lhs_->summary_.always_false || rhs_->summary_.always_false) {
      summary_.always_false = true;
      return;
    }

    if (lhs_->summary_.always_true) {
      summary_ = rhs_->summary_;
    } else if (rhs_->summary_.always_true) {
      summary_ = lhs_->summary_;
    } else {
      for (const auto &literal : lhs_->summary_.positive_literals) {
        if (!addLiteral(literal, true))
          return;
      }
      for (const auto &literal : lhs_->summary_.negative_literals) {
        if (!addLiteral(literal, false))
          return;
      }
      for (const auto &literal : rhs_->summary_.positive_literals) {
        if (!addLiteral(literal, true))
          return;
      }
      for (const auto &literal : rhs_->summary_.negative_literals) {
        if (!addLiteral(literal, false))
          return;
      }
    }

    if (!summary_.always_false) {
      addLiteral(Literal(Literal::Kind::Opaque, nullptr, nullptr, nullptr,
                         nullptr, nullptr, this),
                 true);
    }
  }

  void initializeOrSummary() {
    if (!lhs_ || !rhs_) {
      summary_.always_false = true;
      return;
    }

    if (lhs_->summary_.always_true || rhs_->summary_.always_true) {
      summary_.always_true = true;
      return;
    }

    if (lhs_->summary_.always_false) {
      summary_ = rhs_->summary_;
      return;
    }

    if (rhs_->summary_.always_false) {
      summary_ = lhs_->summary_;
      return;
    }

    for (const auto &literal : lhs_->summary_.positive_literals) {
      if (rhs_->summary_.positive_literals.count(literal) != 0)
        summary_.positive_literals.insert(literal);
    }
    for (const auto &literal : lhs_->summary_.negative_literals) {
      if (rhs_->summary_.negative_literals.count(literal) != 0)
        summary_.negative_literals.insert(literal);
    }

    addLiteral(Literal(Literal::Kind::Opaque, nullptr, nullptr, nullptr,
                       nullptr, nullptr, this),
               true);
  }

public:
  static PathCond *createTrue() {
    PathCond *cond = new PathCond(Kind::True);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createFalse() {
    PathCond *cond = new PathCond(Kind::False);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createValueAtom(Value *value, bool sense) {
    PathCond *cond = new PathCond(value, sense);
    cond->owner_func_ = inferOwner(value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createBranchAtom(BasicBlock *block, BasicBlock *successor,
                                    Value *value, bool sense) {
    PathCond *cond =
        new PathCond(Kind::BranchAtom, block, successor, value, sense,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createSwitchCaseAtom(BasicBlock *block, BasicBlock *successor,
                                        Value *value, ConstantInt *case_value) {
    PathCond *cond =
        new PathCond(Kind::SwitchCaseAtom, block, successor, value, true,
                     block ? block->getParent() : nullptr, case_value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createSwitchDefaultAtom(BasicBlock *block,
                                           BasicBlock *successor,
                                           Value *value) {
    PathCond *cond =
        new PathCond(Kind::SwitchDefaultAtom, block, successor, value, true,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createInvokeNormalAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    PathCond *cond =
        new PathCond(Kind::InvokeNormalAtom, block, successor, nullptr, true,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createInvokeUnwindAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    PathCond *cond =
        new PathCond(Kind::InvokeUnwindAtom, block, successor, nullptr, false,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createBlockAtom(BasicBlock *block) {
    PathCond *cond = new PathCond(block);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createCallTargetAtom(Value *value, Function *callee) {
    PathCond *cond = new PathCond(value, callee);
    cond->owner_func_ = inferOwner(value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createImportedAtom(Function *owner_func, PathCond *imported) {
    PathCond *cond =
        new PathCond(Kind::ImportedAtom, nullptr, nullptr, nullptr, true,
                     owner_func, nullptr, imported, nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createNot(PathCond *inner) {
    PathCond *cond = new PathCond(Kind::Not, inner, nullptr);
    cond->owner_func_ = inner ? inner->getOwnerFunc() : nullptr;
    cond->initializeNotSummary();
    return cond;
  }
  static PathCond *createAnd(PathCond *lhs, PathCond *rhs) {
    PathCond *cond = new PathCond(Kind::And, lhs, rhs);
    if (lhs && rhs && lhs->getOwnerFunc() == rhs->getOwnerFunc())
      cond->owner_func_ = lhs->getOwnerFunc();
    cond->initializeAndSummary();
    return cond;
  }
  static PathCond *createOr(PathCond *lhs, PathCond *rhs) {
    PathCond *cond = new PathCond(Kind::Or, lhs, rhs);
    if (lhs && rhs && lhs->getOwnerFunc() == rhs->getOwnerFunc())
      cond->owner_func_ = lhs->getOwnerFunc();
    cond->initializeOrSummary();
    return cond;
  }

  Kind getKind() const { return kind_; }
  Value *getValue() const { return value_; }
  BasicBlock *getBlock() const { return block_; }
  BasicBlock *getSuccessor() const { return successor_; }
  Function *getCallee() const { return callee_; }
  Function *getOwnerFunc() const { return owner_func_; }
  ConstantInt *getCaseValue() const { return case_value_; }
  PathCond *getImportedSource() const { return imported_; }
  bool getSense() const { return sense_; }
  PathCond *getLhs() const { return lhs_; }
  PathCond *getRhs() const { return rhs_; }
  const ConstraintSummary &getConstraintSummary() const { return summary_; }
  bool isCompound() const {
    return kind_ == Kind::Not || kind_ == Kind::And || kind_ == Kind::Or;
  }

  void print(raw_ostream &OS) const {
    switch (kind_) {
    case Kind::True:
      OS << "true";
      break;
    case Kind::False:
      OS << "false";
      break;
    case Kind::ValueAtom:
      if (!sense_)
        OS << "!";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "<null-cond>";
      }
      break;
    case Kind::BranchAtom:
      OS << "branch(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ", ";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << ", " << (sense_ ? "true" : "false") << ")";
      break;
    case Kind::SwitchCaseAtom:
      OS << "switch-case(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ", ";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " == ";
      if (case_value_)
        case_value_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::SwitchDefaultAtom:
      OS << "switch-default(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::InvokeNormalAtom:
    case Kind::InvokeUnwindAtom:
      OS << (kind_ == Kind::InvokeNormalAtom ? "invoke-normal("
                                             : "invoke-unwind(");
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::BlockAtom:
      OS << "bb(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::CallTargetAtom:
      OS << "calltarget(";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " == ";
      if (callee_)
        OS << callee_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::ImportedAtom:
      OS << "imported(";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " :: ";
      if (callee_)
        OS << callee_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::Not:
      OS << "!(";
      if (lhs_)
        lhs_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::And:
    case Kind::Or:
      OS << "(";
      if (lhs_)
        lhs_->print(OS);
      else
        OS << "null";
      OS << (kind_ == Kind::And ? " && " : " || ");
      if (rhs_)
        rhs_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    }
  }
};

using path_cond_t = PathCond *;

// LLVM value comparator for map/set ordering
struct llvm_cmp {
  bool operator()(const Value *A, const Value *B) const {
    return A < B;
  }
  
  bool operator()(const BasicBlock *A, const BasicBlock *B) const {
    return A < B;
  }
  
  bool operator()(const Function *A, const Function *B) const {
    return A < B;
  }
};

// Singleton for consistent value indexing
class LLVMValueIndex {
  static LLVMValueIndex *Instance;
  LLVMValueIndex() {}
  
public:
  static LLVMValueIndex *get() {
    if (!Instance)
      Instance = new LLVMValueIndex();
    return Instance;
  }
};

} // namespace llvm
