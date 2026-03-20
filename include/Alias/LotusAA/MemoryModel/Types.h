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
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace llvm {

class PathCond {
public:
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
  ConstantInt *case_value_;
  PathCond *imported_;
  bool sense_;
  PathCond *lhs_;
  PathCond *rhs_;

  explicit PathCond(Kind kind)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), case_value_(nullptr), imported_(nullptr),
        sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, bool sense)
      : kind_(Kind::ValueAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(sense), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Kind kind, BasicBlock *block, BasicBlock *successor, Value *value,
           bool sense, ConstantInt *case_value = nullptr,
           PathCond *imported = nullptr, Function *callee = nullptr)
      : kind_(kind), value_(value), block_(block), successor_(successor),
        callee_(callee), case_value_(case_value), imported_(imported),
        sense_(sense), lhs_(nullptr), rhs_(nullptr) {}

  explicit PathCond(BasicBlock *block)
      : kind_(Kind::BlockAtom), value_(nullptr), block_(block),
        successor_(nullptr), callee_(nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, Function *callee)
      : kind_(Kind::CallTargetAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(callee), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Kind kind, PathCond *lhs, PathCond *rhs)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), case_value_(nullptr), imported_(nullptr),
        sense_(true), lhs_(lhs), rhs_(rhs) {}

public:
  static PathCond *createTrue() { return new PathCond(Kind::True); }
  static PathCond *createFalse() { return new PathCond(Kind::False); }
  static PathCond *createValueAtom(Value *value, bool sense) {
    return new PathCond(value, sense);
  }
  static PathCond *createBranchAtom(BasicBlock *block, BasicBlock *successor,
                                    Value *value, bool sense) {
    return new PathCond(Kind::BranchAtom, block, successor, value, sense);
  }
  static PathCond *createSwitchCaseAtom(BasicBlock *block, BasicBlock *successor,
                                        Value *value, ConstantInt *case_value) {
    return new PathCond(Kind::SwitchCaseAtom, block, successor, value, true,
                        case_value);
  }
  static PathCond *createSwitchDefaultAtom(BasicBlock *block,
                                           BasicBlock *successor,
                                           Value *value) {
    return new PathCond(Kind::SwitchDefaultAtom, block, successor, value, true);
  }
  static PathCond *createInvokeNormalAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    return new PathCond(Kind::InvokeNormalAtom, block, successor, nullptr,
                        true);
  }
  static PathCond *createInvokeUnwindAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    return new PathCond(Kind::InvokeUnwindAtom, block, successor, nullptr,
                        false);
  }
  static PathCond *createBlockAtom(BasicBlock *block) {
    return new PathCond(block);
  }
  static PathCond *createCallTargetAtom(Value *value, Function *callee) {
    return new PathCond(value, callee);
  }
  static PathCond *createImportedAtom(Value *callsite, Function *callee,
                                      PathCond *imported) {
    return new PathCond(Kind::ImportedAtom, nullptr, nullptr, callsite, true,
                        nullptr, imported, callee);
  }
  static PathCond *createNot(PathCond *inner) {
    return new PathCond(Kind::Not, inner, nullptr);
  }
  static PathCond *createAnd(PathCond *lhs, PathCond *rhs) {
    return new PathCond(Kind::And, lhs, rhs);
  }
  static PathCond *createOr(PathCond *lhs, PathCond *rhs) {
    return new PathCond(Kind::Or, lhs, rhs);
  }

  Kind getKind() const { return kind_; }
  Value *getValue() const { return value_; }
  BasicBlock *getBlock() const { return block_; }
  BasicBlock *getSuccessor() const { return successor_; }
  Function *getCallee() const { return callee_; }
  ConstantInt *getCaseValue() const { return case_value_; }
  PathCond *getImportedSource() const { return imported_; }
  bool getSense() const { return sense_; }
  PathCond *getLhs() const { return lhs_; }
  PathCond *getRhs() const { return rhs_; }

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
